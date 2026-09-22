#include "bt_companion.h"
#include "board.h"
#include "board_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "bt_companion";

#define BT_COMPANION_NVS_NS  "btcomp"
#define BT_COMPANION_UART    UART_NUM_1
#define BT_COMPANION_BAUD    115200
#define UART_RX_BUF          256

/* ---------------------------------------------------------------------
 * BM64 frame protocol.
 *
 * Host <-> module, both directions, same framing:
 *
 *   byte 0      : 0xAA (sync)
 *   byte 1      : 0x00 (always -- every real capture/reference agrees this
 *                 byte is a fixed zero; likely the high byte of a length
 *                 field wide enough for messages this project never sends)
 *   byte 2      : "len" = 1 (for the opcode/event byte) + number of
 *                 parameter bytes
 *   byte 3      : opcode (host -> module) or event code (module -> host)
 *   byte 4..    : len-1 parameter bytes
 *   last byte   : checksum, chosen so that len + opcode/event + params +
 *                 checksum == 0 (mod 256) -- i.e. two's complement of the
 *                 sum of every byte from the length byte through the last
 *                 parameter byte
 *
 * Not documented anywhere fetchable from here (Microchip's own
 * "UART_CommandSet" document 403'd every attempt) -- reconstructed from
 * https://github.com/konikoni428/BM64_arduino, a working open-source
 * Arduino library that itself cites that same Microchip document. Confirmed
 * self-consistent (TX builder and RX checksum check agree on the exact same
 * checksum radius) but NOT yet confirmed against real hardware replies as
 * of this writing -- that's what bt_companion_raw_ping()/bringup.c's
 * 'bm64_test' console command are for.
 *
 * Every event the module sends (other than an ACK event itself) requires
 * the host to send an explicit ACK command back (MCU_SEND_EVENT_ACK_CMD,
 * carrying the event code as its parameter) -- this is a required part of
 * the handshake, not optional bookkeeping; skipping it is a documented
 * (in the reference library) way to make the module stop responding. */
#define BM64_SYNC 0xAA

typedef enum {
    BM64_MMI_CMD                  = 0x02,
    BM64_READ_LOCAL_BD_ADDR_CMD   = 0x0F,
    BM64_READ_LINK_STATUS_CMD     = 0x0D,
    BM64_GET_UART_VERSION_CMD     = 0x08,
    BM64_MCU_SEND_EVENT_ACK_CMD   = 0x14,
} bm64_cmd_t;

typedef enum {
    BM64_MMI_ANY_MODE_ENTERING_PAIRING = 0x5D,
    BM64_MMI_POWEROFF_BT                = 0x5E, /* not wired to anything yet -- see
                                                  * bt_companion.h's own doc comment on
                                                  * why this needs GPIO-controlled MFB
                                                  * before it's safe to use */
} bm64_mmi_t;

typedef enum {
    BM64_EVENT_ACK                    = 0x00,
    BM64_EVENT_DEVICE_STATE           = 0x01,
    BM64_EVENT_REMOTE_DEVICE_NAME     = 0x17,
    BM64_EVENT_UART_VERSION           = 0x18,
    BM64_EVENT_REPORT_LINK_STATUS     = 0x1E,
    BM64_EVENT_REPORT_LOCAL_BD_ADDR   = 0x20,
} bm64_event_t;

#define POLL_INTERVAL_MS   2000
#define LINK_TIMEOUT_MS    5000  /* no valid frame this long -> link_up = false */
#define RAW_PING_READ_TIMEOUT_MS 20 /* per-byte read timeout while hunting for a frame */

/* Bring-up diagnostics only: counts every byte read_one_frame() sees while
 * hunting for a sync byte (not just ones that end up in a valid frame), and
 * remembers the most recent few. bt_companion_raw_ping() resets this before
 * sending its probe so it can tell "wiring/power/baud dead, zero bytes ever
 * arrived" apart from "bytes arrived but never framed/checksummed correctly"
 * on a failed ping -- those point at very different problems. */
static uint32_t s_dbg_byte_count;
static uint8_t  s_dbg_last_bytes[16];

static bool                  s_enabled = false;
static bool                  s_uart_installed = false;
static TaskHandle_t          s_task;
static int64_t               s_last_reply_us;
static bt_companion_status_t s_status;
static portMUX_TYPE          s_lock = portMUX_INITIALIZER_UNLOCKED;

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(BT_COMPANION_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t en;
    if (nvs_get_u8(h, "enabled", &en) == ESP_OK) s_enabled = (en != 0);
    nvs_close(h);
}

bool bt_companion_enabled(void) { return s_enabled; }

static void start_task(void);
static void stop_task(void);
static esp_err_t ensure_uart(void);

esp_err_t bt_companion_set_enabled(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BT_COMPANION_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "enabled", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    s_enabled = enabled;
    ESP_LOGI(TAG, "%s", enabled ? "enabled -- starting UART link" : "disabled -- stopping UART link");
    if (enabled) start_task();
    else stop_task();
    return ESP_OK;
}

bt_companion_status_t bt_companion_get_status(void)
{
    bt_companion_status_t out;
    portENTER_CRITICAL(&s_lock);
    out = s_status;
    portEXIT_CRITICAL(&s_lock);
    out.enabled = s_enabled;
    if (!s_enabled) { out.link_up = false; out.bt_connected = false; out.paired_name[0] = '\0'; }
    return out;
}

/* ---- low-level frame TX ---- */

static uint8_t frame_checksum(const uint8_t *start, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += start[i];
    return (uint8_t)(~sum + 1);
}

/* Builds and sends one command frame. param_count may be 0 (a bare
 * opcode with no parameters is a valid, common frame -- e.g. a status
 * query that needs no arguments). */
static void send_command(uint8_t opcode, const uint8_t *params, uint8_t param_count)
{
    uint8_t frame[4 + 255 + 1];
    uint8_t len = (uint8_t)(param_count + 1); /* +1 for the opcode byte itself */
    frame[0] = BM64_SYNC;
    frame[1] = 0x00;
    frame[2] = len;
    frame[3] = opcode;
    if (param_count) memcpy(&frame[4], params, param_count);
    frame[4 + param_count] = frame_checksum(&frame[2], (size_t)len + 1);
    uart_write_bytes(BT_COMPANION_UART, (const char *)frame, (size_t)(4 + param_count + 1));
}

static void send_ack(uint8_t event_code)
{
    uint8_t params[2] = { event_code, 0x00 };
    send_command(BM64_MCU_SEND_EVENT_ACK_CMD, params, 2);
}

/* ---- low-level frame RX ----
 * Blocks (with a per-byte timeout) waiting for one complete, checksum-valid
 * frame. Returns true and fills in event_code, params, and param_len on
 * success. params must be at least 254 bytes. Shared by the background poll
 * task and
 * bt_companion_raw_ping() -- callers are responsible for not running both
 * against the UART at once (see bt_companion_raw_ping()'s own doc comment). */
static bool read_one_frame(uint8_t *event_code, uint8_t *params, uint8_t *param_len,
                            int64_t deadline_us)
{
    while (esp_timer_get_time() < deadline_us) {
        uint8_t b;
        if (uart_read_bytes(BT_COMPANION_UART, &b, 1, pdMS_TO_TICKS(RAW_PING_READ_TIMEOUT_MS)) != 1) continue;
        s_dbg_last_bytes[s_dbg_byte_count % (sizeof(s_dbg_last_bytes))] = b;
        s_dbg_byte_count++;
        if (b != BM64_SYNC) continue;

        if (uart_read_bytes(BT_COMPANION_UART, &b, 1, pdMS_TO_TICKS(200)) != 1 || b != 0x00) continue;

        uint8_t len;
        if (uart_read_bytes(BT_COMPANION_UART, &len, 1, pdMS_TO_TICKS(200)) != 1 || len == 0) continue;

        uint8_t code;
        if (uart_read_bytes(BT_COMPANION_UART, &code, 1, pdMS_TO_TICKS(200)) != 1) continue;

        uint8_t plen = (uint8_t)(len - 1);
        uint8_t buf[255];
        bool ok = true;
        for (uint8_t i = 0; i < plen; i++) {
            if (uart_read_bytes(BT_COMPANION_UART, &buf[i], 1, pdMS_TO_TICKS(200)) != 1) { ok = false; break; }
        }
        if (!ok) continue;

        uint8_t checksum;
        if (uart_read_bytes(BT_COMPANION_UART, &checksum, 1, pdMS_TO_TICKS(200)) != 1) continue;

        uint16_t sum = (uint16_t)len + code + checksum;
        for (uint8_t i = 0; i < plen; i++) sum += buf[i];
        if ((sum & 0xFF) != 0) {
            ESP_LOGW(TAG, "frame checksum failed (event/opcode 0x%02X, len %u)", code, len);
            continue;
        }

        *event_code = code;
        *param_len = plen;
        memcpy(params, buf, plen);
        return true;
    }
    return false;
}

/* ---- public commands ---- */

esp_err_t bt_companion_start_pairing(void)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    uint8_t params[2] = { 0x00, BM64_MMI_ANY_MODE_ENTERING_PAIRING };
    send_command(BM64_MMI_CMD, params, 2);
    return ESP_OK;
}

esp_err_t bt_companion_raw_ping_at_baud(int timeout_ms, int baud)
{
    if (s_task) return ESP_ERR_INVALID_STATE; /* background poller would race us for bytes */
    esp_err_t err = ensure_uart();
    if (err != ESP_OK) return err;
    if (baud > 0) {
        err = uart_set_baudrate(BT_COMPANION_UART, (uint32_t)baud);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "probing at %d baud (default is %d)", baud, BT_COMPANION_BAUD);
    }

    s_dbg_byte_count = 0;
    uint8_t params[1] = { 0x00 };
    send_command(BM64_GET_UART_VERSION_CMD, params, 1);
    ESP_LOGI(TAG, "sent GET_UART_VERSION, waiting up to %d ms for a reply...", timeout_ms);

    /* An ACK just confirms the module received the command -- the actual
     * answer (e.g. the UART_VERSION event with the real version string)
     * typically follows as a separate frame right after. Keep draining
     * frames for the whole window instead of returning on the first one, so
     * we see that follow-up too, and ACK every non-ACK event we see (the
     * module can stop responding if a real event goes unacked). */
    uint8_t event_code, param[255], param_len;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int frames_seen = 0;
    while (read_one_frame(&event_code, param, &param_len, deadline)) {
        frames_seen++;
        char hex[3 * 32 + 1] = "";
        int hn = 0;
        for (uint8_t i = 0; i < param_len && i < 32; i++) {
            hn += snprintf(hex + hn, sizeof(hex) - hn, "%02X ", param[i]);
        }
        ESP_LOGI(TAG, "got a valid frame back: event/opcode=0x%02X, %u param byte(s): %s",
                 event_code, param_len, hex);
        if (event_code != BM64_EVENT_ACK) send_ack(event_code);
    }

    if (frames_seen == 0) {
        if (s_dbg_byte_count == 0) {
            ESP_LOGW(TAG, "no valid frame received, and zero raw bytes ever came in on RX -- "
                          "this points at wiring/power/baud, not a framing bug: check TX/RX "
                          "not swapped, shared GND, module actually powered, and BAT_IN above "
                          "its ~2.9V UVP threshold");
        } else {
            char hex[3 * sizeof(s_dbg_last_bytes) + 1] = "";
            int hn = 0;
            uint32_t n = s_dbg_byte_count < sizeof(s_dbg_last_bytes) ? s_dbg_byte_count : sizeof(s_dbg_last_bytes);
            for (uint32_t i = 0; i < n; i++) {
                hn += snprintf(hex + hn, sizeof(hex) - hn, "%02X ", s_dbg_last_bytes[i]);
            }
            ESP_LOGW(TAG, "no valid frame received, but %lu raw byte(s) DID arrive on RX -- "
                          "wiring/power/baud are fine, this looks like a protocol/framing bug "
                          "instead. Last byte(s) seen: %s",
                     (unsigned long)s_dbg_byte_count, hex);
        }
    }
    if (baud > 0) uart_set_baudrate(BT_COMPANION_UART, BT_COMPANION_BAUD);
    return frames_seen > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bt_companion_raw_ping(int timeout_ms)
{
    return bt_companion_raw_ping_at_baud(timeout_ms, 0);
}

esp_err_t bt_companion_read_bd_addr(int timeout_ms, char *out, size_t out_len)
{
    if (s_task) return ESP_ERR_INVALID_STATE; /* background poller would race us for bytes */
    esp_err_t err = ensure_uart();
    if (err != ESP_OK) return err;

    uint8_t params[1] = { 0x00 };
    send_command(BM64_READ_LOCAL_BD_ADDR_CMD, params, 1);
    ESP_LOGI(TAG, "sent READ_LOCAL_BD_ADDR, waiting up to %d ms for a reply...", timeout_ms);

    uint8_t event_code, param[255], param_len;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool found = false;
    while (read_one_frame(&event_code, param, &param_len, deadline)) {
        if (event_code != BM64_EVENT_ACK) send_ack(event_code);
        if (event_code == BM64_EVENT_REPORT_LOCAL_BD_ADDR && param_len >= 6) {
            /* Byte order (MSB vs LSB first) isn't independently confirmed yet --
             * this prints them in wire order. If it doesn't match the address
             * shown on a phone's Bluetooth pairing screen, try reversing it. */
            snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
                     param[0], param[1], param[2], param[3], param[4], param[5]);
            found = true;
        }
    }
    if (!found) ESP_LOGW(TAG, "no BD address reply -- is the module alive? try bm64_test first");
    return found ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ---- background poll task ---- */

static void handle_event(uint8_t event_code, const uint8_t *param, uint8_t param_len)
{
    portENTER_CRITICAL(&s_lock);
    s_status.link_up = true;
    s_last_reply_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);

    switch (event_code) {
    case BM64_EVENT_ACK:
        break; /* nothing to do -- just proves the module got our last command */
    case BM64_EVENT_DEVICE_STATE:
        /* Real semantics of the state byte not yet confirmed against actual
         * hardware (see this component's header/module doc comment) --
         * logged raw so a live bring-up session can correlate this value
         * against real pairing/connect/disconnect actions and calibrate the
         * mapping. Treating "nonzero" as "probably not idle/standby" as a
         * rough placeholder only -- do not trust this for anything beyond
         * "the link is alive" until confirmed live. */
        if (param_len >= 1) {
            ESP_LOGI(TAG, "device state event, raw byte = 0x%02X (mapping not yet confirmed "
                          "against real hardware -- watch this value while pairing/connecting)",
                     param[0]);
            portENTER_CRITICAL(&s_lock);
            s_status.bt_connected = (param[0] != 0x00);
            portEXIT_CRITICAL(&s_lock);
        }
        break;
    case BM64_EVENT_REMOTE_DEVICE_NAME:
        /* An unambiguous signal, unlike DEVICE_STATE above -- the module
         * only has a real remote name to report once something is actually
         * paired/connected. */
        portENTER_CRITICAL(&s_lock);
        s_status.bt_connected = true;
        size_t n = param_len < sizeof(s_status.paired_name) - 1 ? param_len : sizeof(s_status.paired_name) - 1;
        memcpy(s_status.paired_name, param, n);
        s_status.paired_name[n] = '\0';
        portEXIT_CRITICAL(&s_lock);
        break;
    default:
        ESP_LOGD(TAG, "unhandled event 0x%02X (%u param bytes)", event_code, param_len);
        break;
    }

    if (event_code != BM64_EVENT_ACK) send_ack(event_code);
}

static void poll_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(POLL_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();

    /* One-time startup query, not part of the regular poll loop below: grabs
     * the module's own BD address so the web UI can show it (helps whoever's
     * pairing know which device to pick on their phone). Harmless if it
     * times out -- bd_addr just stays "" and everything else still works. */
    {
        uint8_t params[1] = { 0x00 };
        send_command(BM64_READ_LOCAL_BD_ADDR_CMD, params, 1);
        int64_t deadline = esp_timer_get_time() + 500000;
        uint8_t event_code, param[255], param_len;
        while (read_one_frame(&event_code, param, &param_len, deadline)) {
            if (event_code != BM64_EVENT_ACK) send_ack(event_code);
            if (event_code == BM64_EVENT_REPORT_LOCAL_BD_ADDR && param_len >= 6) {
                portENTER_CRITICAL(&s_lock);
                snprintf(s_status.bd_addr, sizeof(s_status.bd_addr),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         param[0], param[1], param[2], param[3], param[4], param[5]);
                portEXIT_CRITICAL(&s_lock);
            }
        }
    }

    while (1) {
        uint8_t params[1] = { 0x00 };
        send_command(BM64_READ_LINK_STATUS_CMD, params, 1);

        int64_t deadline = esp_timer_get_time() + 300000; /* 300ms read window per poll */
        uint8_t event_code, param[255], param_len;
        while (read_one_frame(&event_code, param, &param_len, deadline)) {
            handle_event(event_code, param, param_len);
        }

        portENTER_CRITICAL(&s_lock);
        if (s_status.link_up && esp_timer_get_time() - s_last_reply_us > LINK_TIMEOUT_MS * 1000) {
            s_status.link_up = false;
            s_status.bt_connected = false;
            s_status.paired_name[0] = '\0';
        }
        portEXIT_CRITICAL(&s_lock);

        vTaskDelayUntil(&last_wake, period);
    }
}

static esp_err_t ensure_uart(void)
{
    if (s_uart_installed) return ESP_OK;
    uart_config_t cfg = {
        .baud_rate = BT_COMPANION_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BT_COMPANION_UART, UART_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(BT_COMPANION_UART, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(BT_COMPANION_UART, BOARD_UART_TX, BOARD_UART_RX,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: %s -- companion link won't start", esp_err_to_name(err));
        return err;
    }
    s_uart_installed = true;
    return ESP_OK;
}

static void start_task(void)
{
    if (s_task) return; /* already running */
    if (ensure_uart() != ESP_OK) return;
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_lock);
    xTaskCreate(poll_task, "bt_companion", 3072, NULL, tskIDLE_PRIORITY + 1, &s_task);
}

static void stop_task(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_lock);
    /* UART left installed (cheap, and BOARD_UART_TX/RX have no other
     * claimant) so re-enabling doesn't need to reinstall the driver. */
}

void bt_companion_init(void)
{
    load_config();
    if (s_enabled) start_task();
}
