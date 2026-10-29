#include "bt_companion.h"
#include "bt401.h"
#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
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
    BM64_MAKE_CALL_CMD            = 0x00, /* Data_Base_Index + ASCII number (max 19) */
    BM64_SET_HF_SPEAKER_GAIN_CMD  = 0x1B, /* Data_Base_Index, Gain_Level 0-15 */
    BM64_MMI_CMD                  = 0x02,
    BM64_CHANGE_DEVICE_NAME_CMD   = 0x05,
    BM64_READ_LOCAL_BD_ADDR_CMD   = 0x0F,
    BM64_READ_LINK_STATUS_CMD     = 0x0D,
    BM64_GET_UART_VERSION_CMD     = 0x08,
    BM64_MCU_SEND_EVENT_ACK_CMD   = 0x14,
    /* Takes one Disconnection_Flag byte (see bt_companion_set_enabled()). */
    BM64_DISCONNECT_CMD           = 0x18,
} bm64_cmd_t;

/* BM64_EVENT_DEVICE_STATE's State byte -- confirmed 2026-09-22 straight from
 * Microchip's "Audio UART Command Set" doc (datasheets/AudioUARTCommandSet_v2
 * 02.pdf, section 5.2 BTM_Status), not guessed. Only the values this project
 * actually branches on are named; the rest exist in the real doc too (0x0B
 * AVRCP link established, 0x10 iAP connected, 0x11 ACL disconnected, 0x12-14
 * MAP, 0x15 ACL connected) but nothing here cares yet. */
typedef enum {
    BM64_STATE_POWER_OFF          = 0x00,
    BM64_STATE_PAIRING            = 0x01, /* discoverable mode */
    BM64_STATE_POWER_ON           = 0x02,
    BM64_STATE_PAIRING_OK         = 0x03,
    BM64_STATE_PAIRING_FAILED     = 0x04,
    BM64_STATE_HF_LINK_UP         = 0x05,
    BM64_STATE_A2DP_LINK_UP       = 0x06,
    BM64_STATE_HF_LINK_DOWN       = 0x07,
    BM64_STATE_A2DP_LINK_DOWN     = 0x08,
    BM64_STATE_SCO_UP             = 0x09,
    BM64_STATE_SCO_DOWN           = 0x0A,
    BM64_STATE_STANDBY            = 0x0F,
} bm64_btm_state_t;

typedef enum {
    BM64_MMI_ACCEPT_CALL                = 0x04,
    BM64_MMI_REJECT_CALL                = 0x05,
    BM64_MMI_END_CALL                   = 0x06,
    BM64_MMI_POWERON_BUTTON_PRESS       = 0x51,
    BM64_MMI_POWERON_BUTTON_RELEASE     = 0x52,
    BM64_MMI_ANY_MODE_ENTERING_PAIRING = 0x5D,
    BM64_MMI_POWEROFF_BT                = 0x5E, /* not wired to anything yet -- see
                                                  * bt_companion.h's own doc comment on
                                                  * why this needs GPIO-controlled MFB
                                                  * before it's safe to use */
} bm64_mmi_t;

typedef enum {
    BM64_EVENT_ACK                    = 0x00,
    BM64_EVENT_DEVICE_STATE           = 0x01,
    BM64_EVENT_CALL_STATUS            = 0x02, /* Data_Base_Index, Call_Status */
    BM64_EVENT_CALLER_ID              = 0x03, /* Data_Base_Index, ASCII number */
    BM64_EVENT_REMOTE_DEVICE_NAME     = 0x17,
    BM64_EVENT_UART_VERSION           = 0x18,
    BM64_EVENT_REPORT_LINK_STATUS     = 0x1E,
    BM64_EVENT_HF_GAIN_LEVEL          = 0x0F, /* Data_Base_Index, Gain_Level 0-15 (set by the phone) */
    BM64_EVENT_TYPE_CODEC             = 0x2D, /* Sampling_Frequency, Mode */
    BM64_EVENT_PAIRED_RECORD_REPLY    = 0x1F, /* count, then 7 bytes each: priority + BD addr (LSB first) */
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
/* Set to ask poll_task to exit at its next check, instead of vTaskDelete()ing
 * it from outside. Killing it outright can land while it's blocked inside
 * uart_read_bytes(), which leaves the UART driver's internal read lock held
 * forever -- every later UART call then blocks. Confirmed live 2026-09-26:
 * the boot config-autocheck fires while poll_task is still in its ~3.5s
 * startup sequence, and the old vTaskDelete() path hung the whole check
 * silently right after "IBDK: entering". */
static volatile bool         s_task_stop;
static TaskHandle_t          s_autocheck_task; /* one-shot EEPROM config check */
static bt_companion_call_cb  s_call_cb;
static void                 *s_call_cb_ctx;


static int64_t               s_last_reply_us;
static bt_companion_status_t s_status;
static portMUX_TYPE          s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Per-connected-device call state. The BM64 can hold two devices at once
 * (e.g. a phone and a laptop), and every Call_Status/Caller_ID event says
 * which one it's about (Data_Base_Index). Call-control MMIs must go back to
 * that same device: with a phone mid-call and a laptop ringing, "answer" has
 * to reach the laptop and "hang up" the phone. Guarded by s_lock. */
#define BT_MAX_DEVICES 2
#define BT_CALL_SPEAKER_GAIN 0x0F /* max; see the SCO-up handler */
static bt_call_state_t s_dev_call[BT_MAX_DEVICES];
static char            s_dev_cid[BT_MAX_DEVICES][BT_COMPANION_CALLER_ID_MAX];

static bool call_is_live(bt_call_state_t cs)
{
    return cs == BT_CALL_OUTGOING || cs == BT_CALL_ACTIVE ||
           cs == BT_CALL_ACTIVE_WAITING || cs == BT_CALL_ACTIVE_HOLD;
}

/* The one state reported to the listener, merged across devices. A ringing
 * call wins, since that's what decides whether to ring the bell; otherwise
 * any live call; otherwise whatever is left. Caller must hold s_lock. */
static void merge_call_state_locked(void)
{
    int pick = -1;
    for (int d = 0; d < BT_MAX_DEVICES && pick < 0; d++)
        if (s_dev_call[d] == BT_CALL_INCOMING) pick = d;
    for (int d = 0; d < BT_MAX_DEVICES && pick < 0; d++)
        if (call_is_live(s_dev_call[d])) pick = d;
    for (int d = 0; d < BT_MAX_DEVICES && pick < 0; d++)
        if (s_dev_call[d] != BT_CALL_IDLE) pick = d;
    if (pick < 0) {
        s_status.call_state = BT_CALL_IDLE;
        s_status.caller_id[0] = '\0';
    } else {
        s_status.call_state = s_dev_call[pick];
        memcpy(s_status.caller_id, s_dev_cid[pick], sizeof(s_status.caller_id));
    }
}

static void clear_all_calls_locked(void)
{
    memset(s_dev_call, 0, sizeof(s_dev_call));
    memset(s_dev_cid, 0, sizeof(s_dev_cid));
    merge_call_state_locked();
}

/* Bluetooth is on whenever a module is chosen in Settings -> General -> Board
 * hardware: the module choice is the switch. bt_companion_set_enabled(false)
 * (console `bt_enable 0`, needed by the bm64_* bench commands) pauses it
 * until the next reboot. A BT401 also has its own switch (Settings ->
 * Bluetooth) that is kept across reboots: off puts the module in idle mode
 * (radio off), which saves ~25 mA. */
static board_bt_module_t module(void);
static void load_config(void)
{
    s_enabled = board_hw_config_get().has_bt_module;
    if (s_enabled && module() == BOARD_BT_BT401) {
        nvs_handle_t h;
        uint8_t v = 1;
        if (nvs_open(BT_COMPANION_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "enabled", &v);
            nvs_close(h);
        }
        s_enabled = v != 0;
    }
}

/* The two EEPROM-programming leads (carrier J2 pin 1 = P2_0, pin 2 = RST_N)
 * are documented on GPIO20 / GPIO19 (header J5's odd pins 3 and 5), but a
 * wire can land on the wrong pin. The pins are runtime values kept in NVS
 * (`bm64_diagpins` sets them; `bm64_findwire` finds RST_N). If IBDK entry
 * fails the documented way it is tried once with the two swapped, and a swap
 * that works is remembered. */
#ifdef BOARD_BT_MFB_PIN
static int  s_rst_pin = -1, s_p20_pin = -1;
static bool s_diag_tried_swap;
static void diag_pins_load(void)
{
    if (s_rst_pin >= 0) return;
    s_rst_pin = BOARD_BT_RST_PIN;
    s_p20_pin = BOARD_BT_P20_PIN;
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        uint8_t r, p;
        if (nvs_get_u8(h, "bt_rst_pin", &r) == ESP_OK && nvs_get_u8(h, "bt_p20_pin", &p) == ESP_OK &&
            r >= 18 && r <= 21 && p >= 18 && p <= 21 && r != p) {
            s_rst_pin = r;
            s_p20_pin = p;
        }
        nvs_close(h);
    }
}
static int diag_rst_pin(void) { diag_pins_load(); return s_rst_pin; }
static int diag_p20_pin(void) { diag_pins_load(); return s_p20_pin; }
static void diag_pins_save(void)
{
    nvs_handle_t h;
    if (nvs_open("board", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bt_rst_pin", (uint8_t)s_rst_pin);
    nvs_set_u8(h, "bt_p20_pin", (uint8_t)s_p20_pin);
    nvs_commit(h);
    nvs_close(h);
}
esp_err_t bt_companion_diag_pins_set(int rst, int p20)
{
    if (rst < 18 || rst > 21 || p20 < 18 || p20 > 21 || rst == p20) return ESP_ERR_INVALID_ARG;
    diag_pins_load();
    s_rst_pin = rst;
    s_p20_pin = p20;
    diag_pins_save();
    return ESP_OK;
}
void bt_companion_diag_pins_get(int *rst, int *p20)
{
    if (rst) *rst = diag_rst_pin();
    if (p20) *p20 = diag_p20_pin();
}
#endif /* BOARD_BT_MFB_PIN: boards without the mode-pin wiring get stubs further down */

/* Which module is on the header. Fixed at boot (Settings -> Hardware takes
 * effect on reboot), so the BM64 and BT401 backends never have to share the
 * UART or hand over mid-run. */
static board_bt_module_t s_module = BOARD_BT_NONE;
static bool              s_module_known;

static board_bt_module_t module(void)
{
    if (!s_module_known) {
        s_module = board_hw_config_get().bt_module;
        s_module_known = true;
    }
    return s_module;
}

board_bt_module_t bt_companion_module(void) { return module(); }

bool bt_companion_can_dial(void) { return module() == BOARD_BT_BM64 || module() == BOARD_BT_BT401; }

static void bt401_notify(bt_call_state_t st)
{
    if (s_call_cb) s_call_cb(st, s_call_cb_ctx);
}

bool bt_companion_enabled(void) { return s_enabled; }

bool bt_companion_active(void)
{
    return s_enabled && board_hw_config_get().has_bt_module;
}

/* ---- BT401 sleep timer ---- */

static uint8_t          s_sleep_hours = BT_COMPANION_SLEEP_HOURS_DEFAULT;
static volatile bool    s_bt401_asleep;
static volatile bool    s_wake_req, s_pair_on_wake, s_sleep_req;
static int64_t          s_last_phone_us;   /* last time a phone was connected (or the timer restarted) */
static TaskHandle_t     s_sleep_timer_task;

uint8_t bt_companion_sleep_hours_get(void) { return s_sleep_hours; }

esp_err_t bt_companion_sleep_hours_set(uint8_t hours)
{
    if (hours > 24) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(BT_COMPANION_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "sleep_h", hours);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    s_sleep_hours = hours;
    s_last_phone_us = esp_timer_get_time();   /* a new setting starts a fresh wait */
    if (hours == 0) bt_companion_wake();
    return ESP_OK;
}

void bt_companion_wake(void)
{
    if (!s_bt401_asleep || !s_sleep_timer_task) return;
    s_wake_req = true;
    xTaskNotifyGive(s_sleep_timer_task);
}

/* Wakes are handed to this task because bt401_start() can wait for a sleep
 * sequence still in progress; the callers (telephony's FSM) must not block. */
esp_err_t bt_companion_sleep_now(void)
{
    if (module() != BOARD_BT_BT401 || !s_sleep_timer_task || !s_enabled || s_bt401_asleep)
        return ESP_ERR_INVALID_STATE;
    s_sleep_req = true;
    xTaskNotifyGive(s_sleep_timer_task);
    return ESP_OK;
}

static void sleep_timer_task(void *arg)
{
    (void)arg;
    s_last_phone_us = esp_timer_get_time();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
        int64_t now = esp_timer_get_time();
        if (s_wake_req) {
            s_wake_req = false;
            if (s_bt401_asleep && s_enabled) {
                ESP_LOGI(TAG, "waking the BT401 (radio back on)");
                s_bt401_asleep = false;
                s_last_phone_us = now;
                bt401_start(bt401_notify);
                if (s_pair_on_wake) bt401_pair_when_ready();
            }
            s_pair_on_wake = false;
            continue;
        }
        if (s_sleep_req && s_enabled && !s_bt401_asleep) {
            s_sleep_req = false;
            ESP_LOGI(TAG, "BT401 to idle mode now (console) until a handset is lifted");
            s_bt401_asleep = true;
            bt401_stop(true);
            bt401_sleep();
            continue;
        }
        s_sleep_req = false;
        if (!s_enabled || s_bt401_asleep || !bt401_running() || s_sleep_hours == 0) {
            if (!s_bt401_asleep) s_last_phone_us = now;
            continue;
        }
        bt_companion_status_t st;
        bt401_get_status(&st);
        if (st.bt_connected || st.call_state != BT_CALL_IDLE) {
            s_last_phone_us = now;
            continue;
        }
        if (now - s_last_phone_us >= (int64_t)s_sleep_hours * 3600 * 1000000LL) {
            ESP_LOGI(TAG, "no phone connected for %u h -- BT401 to idle mode until a handset is lifted",
                     (unsigned)s_sleep_hours);
            s_bt401_asleep = true;
            bt401_stop(false);
            bt401_sleep();
        }
    }
}

static void start_task(void);
static void stop_task(void);
static void send_command(uint8_t opcode, const uint8_t *params, uint8_t param_count);
static void start_autocheck(void);
#ifdef BOARD_BT_MFB_PIN
static volatile bool s_mfb_held;
static void mfb_hold(bool on);
static void mfb_before_tx(void);
static void mfb_after_tx(void);
#endif
static esp_err_t ensure_uart(void);
static esp_err_t ensure_uart_peripheral_only(void);

esp_err_t bt_companion_set_enabled(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BT_COMPANION_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "enabled", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    if (module() == BOARD_BT_BT401) {
        s_enabled = enabled;
        if (enabled) {
            ESP_LOGI(TAG, "enabled -- starting BT401 link");
            s_bt401_asleep = false;
            s_last_phone_us = esp_timer_get_time();
            return bt401_start(bt401_notify);
        }
        ESP_LOGI(TAG, "disabled -- disconnecting, stopping BT401 link, module to idle mode");
        bool was_asleep = s_bt401_asleep;
        s_bt401_asleep = false;
        if (was_asleep) return ESP_OK;   /* already in idle mode */
        bt401_stop(true);
        return bt401_sleep();
    }

    if (enabled) {
        s_enabled = true;
        ESP_LOGI(TAG, "enabled -- starting UART link");
        start_task();
        start_autocheck();
        return ESP_OK;
    }

    /* Turning the feature off has to actually tear the Bluetooth link down,
     * not just stop us listening to it. Merely stopping the poll task leaves
     * the module still paired, still connected, and still driving the
     * analog audio node -- which on 2026-09-26 looked exactly like "I turned
     * Bluetooth off and the phone still has no dial tone", because the BM64
     * was happily streaming into the SLIC's Vin the whole time.
     *
     * Disconnect (0x18) flag bits: 0 = cancel a page still in progress,
     * 1 = drop all HF links, 2 = drop all A2DP links (Microchip Audio UART
     * Command Set 5.1.22). Sent before the task stops so the module is still
     * being serviced, and best-effort -- if the link is already down there's
     * nothing to disconnect and the module just NAKs it. */
    portENTER_CRITICAL(&s_lock);
    bool was_linked = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (was_linked && s_uart_installed) {
        uint8_t flags[1] = { 0x07 };
        send_command(BM64_DISCONNECT_CMD, flags, 1);
        vTaskDelay(pdMS_TO_TICKS(150)); /* let it go out and be acted on */
    }

    s_enabled = false;
    ESP_LOGI(TAG, "disabled -- disconnected and stopping UART link");
    stop_task();
    return ESP_OK;
}

bt_companion_status_t bt_companion_get_status(void)
{
    bt_companion_status_t out;
    if (module() == BOARD_BT_BT401) {
        bt401_get_status(&out);
        out.enabled = s_enabled;
        out.asleep = s_enabled && s_bt401_asleep;
        if (!s_enabled) { out.link_up = false; out.bt_connected = false; out.paired_name[0] = '\0'; }
        return out;
    }
    portENTER_CRITICAL(&s_lock);
    out = s_status;
    portEXIT_CRITICAL(&s_lock);
    out.enabled = s_enabled;
    out.asleep = false;
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
#ifdef BOARD_BT_MFB_PIN
    mfb_before_tx();
#endif
    uart_write_bytes(BT_COMPANION_UART, (const char *)frame, (size_t)(4 + param_count + 1));
#ifdef BOARD_BT_MFB_PIN
    mfb_after_tx();
#endif
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
    if (module() == BOARD_BT_BT401) {
        if (!s_enabled) return ESP_ERR_INVALID_STATE;
        if (s_bt401_asleep) {      /* wake it, then pair once it's back in Bluetooth mode */
            s_pair_on_wake = true;
            bt_companion_wake();
            board_led_set_state(BOARD_LED_BT_PAIRING);
            return ESP_OK;
        }
        esp_err_t r = bt401_start_pairing();
        if (r == ESP_OK) board_led_set_state(BOARD_LED_BT_PAIRING);
        return r;
    }
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    /* Confirmed 2026-09-22 on real hardware: sending the pairing MMI action
     * (0x5D) alone gets ACKed (status 0x00) but the module never actually
     * broadcasts as discoverable -- no BM64_EVENT_DEVICE_STATE follows at
     * all. The missing piece, straight from the BM64_arduino library's (https://github.com/konikoni428/BM64_arduino) own
     * powerOn(): a POWERON_BUTTON_PRESS (0x51) then POWERON_BUTTON_RELEASE
     * (0x52) MMI action pair first. Doing that (even with MFB itself just
     * tied high on the bench, no GPIO control) is what got the module to
     * actually emit DEVICE_STATE 0x02 (Power ON) -> 0x0F (Standby), and
     * only *after* that did the pairing command that followed produce the
     * expected DEVICE_STATE 0x01 (Pairing state / discoverable mode). Cheap
     * insurance to always send this pair first -- harmless if the module
     * was already past this point. */
    uint8_t press[2]   = { 0x00, BM64_MMI_POWERON_BUTTON_PRESS };
    uint8_t release[2] = { 0x00, BM64_MMI_POWERON_BUTTON_RELEASE };
    send_command(BM64_MMI_CMD, press, 2);
    vTaskDelay(pdMS_TO_TICKS(50));
    send_command(BM64_MMI_CMD, release, 2);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* Re-send the device name right before entering discoverable mode,
     * in case the startup send was too early and got ignored. */
    {
        char name[32] = {0};
        board_name_get(name, sizeof(name));
        send_command(BM64_CHANGE_DEVICE_NAME_CMD, (const uint8_t *)name, sizeof(name));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    uint8_t params[2] = { 0x00, BM64_MMI_ANY_MODE_ENTERING_PAIRING };
    send_command(BM64_MMI_CMD, params, 2);
    board_led_set_state(BOARD_LED_BT_PAIRING);
    return ESP_OK;
}

esp_err_t bt_companion_send_mmi(uint8_t action)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    uint8_t params[2] = { 0x00, action };
    send_command(BM64_MMI_CMD, params, 2);
    /* Reply (ACK, and whatever else it triggers) surfaces via the
     * background poller's own read loop -> handle_event()'s ACK logging,
     * same as bt_companion_start_pairing() -- not read here. */
    return ESP_OK;
}

void bt_companion_set_call_cb(bt_companion_call_cb cb, void *ctx)
{
    s_call_cb_ctx = ctx;
    s_call_cb = cb;
}

/* Sends a call-control MMI to device `dev` (the MMI's data_base_index,
 * bits 0-2). Same preconditions as bt_companion_send_mmi(). */
/* Which database index has its HF profile up -- from the poller's
 * Read_Link_Status_Reply (0x1E, Database0/1_Connect_Status bit 3). Make_Call
 * has to name a device, and a call needs HF (A2DP alone can't place one). */
static bool s_dev_hf[BT_MAX_DEVICES];
/* Per database: the phone is streaming audio (Read_Link_Status_Reply's Stream_Status). */
static bool s_dev_stream[BT_MAX_DEVICES];

esp_err_t bt_companion_make_call(const char *number)
{
    if (module() == BOARD_BT_BT401) return s_enabled ? bt401_dial(number) : ESP_ERR_INVALID_STATE;
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    size_t len = number ? strlen(number) : 0;
    if (len == 0 || len > 19) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up && s_status.bt_connected;
    int dev = -1;
    for (int d = 0; d < BT_MAX_DEVICES && dev < 0; d++)
        if (s_dev_hf[d]) dev = d;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    if (dev < 0) dev = 0; /* no link-status reply yet -- the usual single device */
    uint8_t params[1 + 19];
    params[0] = (uint8_t)dev;
    memcpy(&params[1], number, len);
    send_command(BM64_MAKE_CALL_CMD, params, (uint8_t)(1 + len));
    ESP_LOGI(TAG, "make call via device %d: %s", dev, number);
    return ESP_OK;
}

static esp_err_t send_mmi_to(int dev, uint8_t action)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    uint8_t params[2] = { (uint8_t)(dev & 0x07), action };
    send_command(BM64_MMI_CMD, params, 2);
    return ESP_OK;
}

/* First device whose call satisfies `want`, or -1. */
static int find_device(bool (*want)(bt_call_state_t))
{
    int found = -1;
    portENTER_CRITICAL(&s_lock);
    for (int d = 0; d < BT_MAX_DEVICES && found < 0; d++)
        if (want(s_dev_call[d])) found = d;
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static bool call_is_incoming(bt_call_state_t cs) { return cs == BT_CALL_INCOMING; }

esp_err_t bt_companion_accept_call(void)
{
    if (module() == BOARD_BT_BT401) return s_enabled ? bt401_accept_call() : ESP_ERR_INVALID_STATE;
    int d = find_device(call_is_incoming);
    return d < 0 ? ESP_ERR_NOT_FOUND : send_mmi_to(d, BM64_MMI_ACCEPT_CALL);
}

esp_err_t bt_companion_reject_call(void)
{
    if (module() == BOARD_BT_BT401) return s_enabled ? bt401_reject_call() : ESP_ERR_INVALID_STATE;
    int d = find_device(call_is_incoming);
    return d < 0 ? ESP_ERR_NOT_FOUND : send_mmi_to(d, BM64_MMI_REJECT_CALL);
}

esp_err_t bt_companion_end_call(void)
{
    if (module() == BOARD_BT_BT401) return s_enabled ? bt401_end_call() : ESP_ERR_INVALID_STATE;
    int d = find_device(call_is_live);
    return d < 0 ? ESP_ERR_NOT_FOUND : send_mmi_to(d, BM64_MMI_END_CALL);
}

bool bt_companion_music_playing(void)
{
    if (module() == BOARD_BT_BT401) return s_enabled && bt401_music_playing();
    portENTER_CRITICAL(&s_lock);
    bool r = s_status.bt_connected && (s_dev_stream[0] || s_dev_stream[1]);
    portEXIT_CRITICAL(&s_lock);
    return r;
}

bool bt_companion_call_live(void)
{
    if (module() == BOARD_BT_BT401) return s_enabled && bt401_call_live();
    return find_device(call_is_live) >= 0;
}

esp_err_t bt_companion_send_cmd(uint8_t opcode, const uint8_t *params, uint8_t param_count)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    /* Framing only -- like bt_companion_send_raw_cmd()/bt_companion_send_mmi(),
     * the reply surfaces via the background poller's own read loop, not here.
     * Unlike those two (which only ever need a fixed 1-2 byte payload), this
     * is the general entry point for real command-set commands that take
     * their own multi-byte parameter list -- e.g. Set_Overall_Gain (0x23),
     * Data_Base_Index/Mask/Type/Gain1/Gain2/Gain3, 6 bytes, per Microchip's
     * Audio UART Command Set doc (Bluetooth/Docs/, section 5.1.30). */
    send_command(opcode, params, param_count);
    return ESP_OK;
}

esp_err_t bt_companion_send_raw_cmd(uint8_t opcode, uint8_t reserved_byte)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    /* Every simple "read info" command in the real command set (Read_Local_
     * BD_Address, Read_Local_Device_Name, Read_Paired_Device_Record, ...)
     * takes exactly this shape: opcode + one reserved byte, no other
     * parameters. Reply surfaces the same way as bt_companion_send_mmi()'s
     * does -- via the background poller -> handle_event()'s "unhandled
     * event" hex dump, since this is deliberately generic rather than
     * hardcoding a parser for whichever specific reply comes back. */
    uint8_t params[1] = { reserved_byte };
    send_command(opcode, params, 1);
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

/* Sends arbitrary raw bytes (no AA00 framing, no checksum -- for probing
 * BM64's undocumented IBDK/test-mode protocol, which isn't the normal
 * command set at all) and prints whatever comes back for listen_ms, live,
 * same style as bt_companion_raw_snoop(). Deliberately goes through
 * ensure_uart_peripheral_only() rather than ensure_uart() -- IBDK mode
 * needs P2_0 held LOW, the opposite of what ensure_uart()'s normal-mode
 * pin assertion forces, so pin control here is 100% left to the caller
 * (bringup.c's bm64_pin command) rather than being touched automatically. */
esp_err_t bt_companion_raw_send(const uint8_t *tx, size_t tx_len, int listen_ms)
{
    if (s_task) return ESP_ERR_INVALID_STATE; /* background poller would race us for bytes */
    esp_err_t err = ensure_uart_peripheral_only();
    if (err != ESP_OK) return err;

    uart_flush_input(BT_COMPANION_UART);
    printf("TX: ");
    for (size_t i = 0; i < tx_len; i++) printf("%02X ", tx[i]);
    printf("\n");
    uart_write_bytes(BT_COMPANION_UART, (const char *)tx, tx_len);

    printf("RX (%d ms): ", listen_ms);
    int64_t deadline = esp_timer_get_time() + (int64_t)listen_ms * 1000;
    uint32_t count = 0;
    while (esp_timer_get_time() < deadline) {
        uint8_t b;
        if (uart_read_bytes(BT_COMPANION_UART, &b, 1, pdMS_TO_TICKS(20)) == 1) {
            printf("%02X ", b);
            fflush(stdout);
            count++;
        }
    }
    printf("\n%lu byte(s) received\n", (unsigned long)count);
    return ESP_OK;
}

int bt_companion_raw_xfer(const uint8_t *tx, size_t tx_len,
                          uint8_t *rx_buf, size_t rx_buf_len,
                          int listen_ms)
{
    if (s_task) return -1;
    esp_err_t err = ensure_uart_peripheral_only();
    if (err != ESP_OK) return -1;

    uart_flush_input(BT_COMPANION_UART);
    uart_write_bytes(BT_COMPANION_UART, (const char *)tx, tx_len);

    int64_t deadline = esp_timer_get_time() + (int64_t)listen_ms * 1000;
    size_t count = 0;
    while (esp_timer_get_time() < deadline) {
        uint8_t b;
        if (uart_read_bytes(BT_COMPANION_UART, &b, 1, pdMS_TO_TICKS(20)) == 1) {
            if (count < rx_buf_len) rx_buf[count] = b;
            count++;
        }
    }
    return (int)count;
}

esp_err_t bt_companion_raw_snoop(int duration_ms)
{
    if (s_task) return ESP_ERR_INVALID_STATE; /* background poller would race us for bytes */
    esp_err_t err = ensure_uart();
    if (err != ESP_OK) return err;

    /* Deliberately no framing/checksum logic at all here -- unlike every
     * other bring-up command, this prints every raw byte the instant it
     * arrives, live, with no waiting for a complete valid frame first.
     * That's the whole point: a bad/glitchy signal (or nothing at all)
     * shows up immediately as you watch, instead of silently vanishing
     * into read_one_frame()'s validation before you'd ever see it. */
    printf("snooping raw RX bytes for %d ms (Ctrl-T Ctrl-R doesn't stop this -- just wait it "
           "out)...\n", duration_ms);
    int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    uint32_t count = 0;
    while (esp_timer_get_time() < deadline) {
        uint8_t b;
        if (uart_read_bytes(BT_COMPANION_UART, &b, 1, pdMS_TO_TICKS(20)) == 1) {
            printf("%02X ", b);
            fflush(stdout);
            count++;
            if (count % 16 == 0) printf("\n");
        }
    }
    printf("\nsnoop done: %lu byte(s) seen\n", (unsigned long)count);
    return ESP_OK;
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
        /* params = {opcode being acknowledged, status byte} (confirmed
         * 2026-09-21 on real hardware). Stay silent for the routine poll
         * command (BM64_READ_LINK_STATUS_CMD, every 2s) -- that would just
         * be log spam -- but log anything else at INFO, since that's
         * always a foreground command a caller is actively waiting to see
         * the result of (e.g. bt_companion_start_pairing()'s MMI_CMD),
         * and this was the only way to tell whether the module actually
         * accepted it. Status byte meaning not independently confirmed --
         * watch for whether it's ever nonzero to find out empirically. */
        if (param_len >= 2 && param[0] != BM64_READ_LINK_STATUS_CMD) {
            ESP_LOGI(TAG, "ACK for opcode 0x%02X, status byte 0x%02X", param[0], param[1]);
        }
        break;
    case BM64_EVENT_DEVICE_STATE:
        /* State byte meaning confirmed 2026-09-22 from Microchip's real
         * command-set doc (BTM_Status, event 0x01) -- see the bm64_btm_state_t
         * enum above. HF/A2DP/SCO link-up states are the actual "something is
         * connected" signal; PAIRING_OK alone doesn't necessarily mean a
         * profile link is up yet, so it's logged but doesn't set
         * bt_connected on its own. */
        if (param_len >= 1) {
            static const char *const names[] = {
                "power off", "pairing (discoverable)", "power on", "pairing OK",
                "pairing failed", "HF link up", "A2DP link up", "HF link down",
                "A2DP link down", "call audio (SCO) up", "call audio (SCO) down",
                "AVRCP link up", "AVRCP link down", "SPP up", "SPP down", "standby",
                "iAP up", "ACL disconnected", "MAP up", "MAP forbidden", "MAP down",
                "ACL connected", "SPP/iAP down", "link back (paging)", "inquiry",
            };
            ESP_LOGI(TAG, "device state event: 0x%02X (%s)", param[0],
                     param[0] < sizeof(names) / sizeof(names[0]) ? names[param[0]] : "other");
            switch (param[0]) {
            case BM64_STATE_PAIRING:
                board_led_set_state(BOARD_LED_BT_PAIRING);
                break;
            case BM64_STATE_HF_LINK_UP:
            case BM64_STATE_A2DP_LINK_UP:
                portENTER_CRITICAL(&s_lock);
                s_status.bt_connected = true;
                portEXIT_CRITICAL(&s_lock);
                board_led_set_state(BOARD_LED_BT_CONNECTED);
                break;
            case BM64_STATE_SCO_UP: {
                /* Call audio just started. The phone restores its remembered
                 * call volume for this headset (level 4 of 15 on the first
                 * real test call -- audible only faintly, and invisible on a
                 * scope at Vin). A rotary handset has no volume control, so
                 * pin it to max; the phone's own volume buttons still work
                 * mid-call. Max is ~1.1 Vpp at AOHPL, well inside what the
                 * SLIC's Vin takes from the codec. */
                int dev = 0;
                portENTER_CRITICAL(&s_lock);
                for (int d = 0; d < BT_MAX_DEVICES; d++)
                    if (s_dev_call[d] != BT_CALL_IDLE) { dev = d; break; }
                portEXIT_CRITICAL(&s_lock);
                uint8_t gain[2] = { (uint8_t)dev, BT_CALL_SPEAKER_GAIN };
                send_command(BM64_SET_HF_SPEAKER_GAIN_CMD, gain, 2);
                break;
            }
            case BM64_STATE_HF_LINK_DOWN:
            case BM64_STATE_A2DP_LINK_DOWN: {
                portENTER_CRITICAL(&s_lock);
                s_status.bt_connected = false;
                s_status.paired_name[0] = '\0';
                /* Losing the HF link ends any call -- without this a bell
                 * started for an incoming call would ring on forever. */
                /* The event doesn't say which device dropped, so every call
                 * is cleared -- right for the usual one-device case; with two
                 * connected it's conservative (the survivor re-reports its
                 * state on its next change). */
                bool had_call = (param[0] == BM64_STATE_HF_LINK_DOWN &&
                                 s_status.call_state != BT_CALL_IDLE);
                if (had_call) clear_all_calls_locked();
                portEXIT_CRITICAL(&s_lock);
                if (had_call && s_call_cb) s_call_cb(BT_CALL_IDLE, s_call_cb_ctx);
                break;
            }
            default:
                break;
            }
        }
        break;
    case BM64_EVENT_CALL_STATUS:
        if (param_len >= 2) {
            int dev = param[0] & 0x07;
            if (dev >= BT_MAX_DEVICES) break;
            bt_call_state_t cs = (bt_call_state_t)param[1];
            portENTER_CRITICAL(&s_lock);
            bt_call_state_t prev = s_status.call_state;
            s_dev_call[dev] = cs;
            if (cs == BT_CALL_IDLE) s_dev_cid[dev][0] = '\0';
            merge_call_state_locked();
            bt_call_state_t now = s_status.call_state;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "device %d call state -> %d (reported %d -> %d)", dev, cs, prev, now);
            if (now != prev && s_call_cb) s_call_cb(now, s_call_cb_ctx);
        }
        break;
    case BM64_EVENT_CALLER_ID:
        if (param_len >= 2) {
            int dev = param[0] & 0x07;
            if (dev >= BT_MAX_DEVICES) break;
            portENTER_CRITICAL(&s_lock);
            size_t cn = (size_t)(param_len - 1);
            if (cn > sizeof(s_dev_cid[dev]) - 1) cn = sizeof(s_dev_cid[dev]) - 1;
            memcpy(s_dev_cid[dev], &param[1], cn);
            s_dev_cid[dev][cn] = '\0';
            merge_call_state_locked();
            bt_call_state_t cs = s_status.call_state;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "caller ID: %s", s_status.caller_id);
            /* Same state, re-notified so a listener already ringing can pick
             * up the number, which often arrives after the ring starts. */
            if (s_call_cb) s_call_cb(cs, s_call_cb_ctx);
        }
        break;
    case BM64_EVENT_REPORT_LINK_STATUS:
        /* Device_State, DB0_Connect, DB1_Connect, ... -- bit 3 = HF up.
         * Device_State 3/4/5/6 = connected (HF / A2DP / SPP / multi).
         * Also the source of truth for bt_connected: with the BM64 no longer
         * reset when the ESP32 reboots, a phone stays linked across our
         * reboot and no link-up event ever arrives -- without this we'd
         * report "no phone connected" and refuse to dial out. */
        if (param_len >= 3) {
            bool conn = param[0] >= 0x03 && param[0] <= 0x06;
            portENTER_CRITICAL(&s_lock);
            s_dev_hf[0] = (param[1] & 0x08) != 0;
            s_dev_hf[1] = (param[2] & 0x08) != 0;
            s_dev_stream[0] = param_len >= 7 && conn && param[5] == 0x01;
            s_dev_stream[1] = param_len >= 7 && conn && param[6] == 0x01;
            bool was = s_status.bt_connected;
            s_status.bt_connected = conn;
            if (!conn) s_status.paired_name[0] = '\0';
            portEXIT_CRITICAL(&s_lock);
            if (conn != was) {
                ESP_LOGI(TAG, "link status: phone %s", conn ? "connected" : "not connected");
                if (conn) board_led_set_state(BOARD_LED_BT_CONNECTED);
            }
        }
        break;
    case BM64_EVENT_HF_GAIN_LEVEL:
        if (param_len >= 2)
            ESP_LOGI(TAG, "device %d call speaker level %u/15 (set by the phone)", param[0] & 0x07, param[1]);
        break;
    case BM64_EVENT_TYPE_CODEC:
        if (param_len >= 2 && param[1] == 0x06)
            ESP_LOGI(TAG, "call audio codec: %s", param[0] == 0x02 ? "16 kHz wideband (mSBC)"
                                                   : param[0] == 0x00 ? "8 kHz narrowband (CVSD)" : "other rate");
        else
            ESP_LOGD(TAG, "codec mode 0x%02X rate 0x%02X", param_len >= 2 ? param[1] : 0, param[0]);
        break;
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: /* phone battery/signal/roaming/service */
    case 0x1A: /* AVRCP vendor response */
    case 0x24: /* ringtone status */
    case 0x32: /* LE signaling */
        ESP_LOGD(TAG, "phone status event 0x%02X", event_code);
        break;
    case 0x1B: /* BTM_Utility_Req -- Action_Type first; 0x03 = EEPROM update finished */
        if (param_len >= 1 && param[0] == 0x03)
            ESP_LOGI(TAG, "module finished writing its EEPROM (pairings are saved at power-off)");
        else
            ESP_LOGI(TAG, "module request to MCU: action 0x%02X", param_len ? param[0] : 0);
        break;
    case BM64_EVENT_PAIRED_RECORD_REPLY:
        /* Reply to Read_Paired_Device_Record (bm64_cmd 0E) -- logged at INFO
         * because "did the module keep its pairing across a reboot" is the
         * open question behind phones failing to reconnect. */
        if (param_len >= 1) {
            ESP_LOGI(TAG, "paired device records: %u", param[0]);
            for (int i = 0; i < param[0] && 1 + 7 * (i + 1) <= param_len; i++) {
                const uint8_t *r = &param[1 + 7 * i];
                ESP_LOGI(TAG, "  #%d priority %u  %02X:%02X:%02X:%02X:%02X:%02X", i, r[0],
                         r[6], r[5], r[4], r[3], r[2], r[1]);
            }
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
    default: {
        /* Hex-dumped so a one-off diagnostic command (bm64_cmd) doesn't need
         * bespoke parsing added here just to see what came back -- this is
         * the ONLY place that would ever show it otherwise. */
        char hex[3 * 32 + 1] = "";
        int hn = 0;
        for (uint8_t i = 0; i < param_len && i < 32; i++) {
            hn += snprintf(hex + hn, sizeof(hex) - hn, "%02X ", param[i]);
        }
        /* INFO, not DEBUG: the 2 s link-status poll reply is handled above,
         * so what lands here is rare and usually exactly what's being chased. */
        ESP_LOGI(TAG, "unhandled event 0x%02X (%u param bytes): %s", event_code, param_len, hex);
        break;
    }
    }

    if (event_code != BM64_EVENT_ACK) send_ack(event_code);
}

static void poll_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(POLL_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();

    /* One-time at startup, before anything else: confirmed 2026-09-22 this
     * is what actually gets the module from whatever state it's in after
     * MFB power-on into one where later commands (pairing especially)
     * genuinely take effect, not just get ACKed -- see
     * bt_companion_start_pairing()'s own comment for the full story. */
    {
#ifdef BOARD_BT_MFB_PIN
        /* Datasheet Fig 5-3/5-6: MFB high, 400 ms, then the power-on command. */
        mfb_hold(true);
        vTaskDelay(pdMS_TO_TICKS(400));
#endif
        uint8_t press[2]   = { 0x00, BM64_MMI_POWERON_BUTTON_PRESS };
        uint8_t release[2] = { 0x00, BM64_MMI_POWERON_BUTTON_RELEASE };
        send_command(BM64_MMI_CMD, press, 2);
        vTaskDelay(pdMS_TO_TICKS(50));
        send_command(BM64_MMI_CMD, release, 2);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Wait for the module to reach Power ON / Standby before sending any
     * config commands -- the name-change especially gets silently ignored
     * if it arrives before the module has finished its own power-on
     * sequence. Drain events for up to 3 seconds, looking for DEVICE_STATE
     * 0x02 (Power ON) or 0x0F (Standby). Harmless if it times out. */
    {
        int64_t deadline = esp_timer_get_time() + 3000000; /* 3s */
        uint8_t event_code, param[255], param_len;
        bool ready = false;
        while (!ready && read_one_frame(&event_code, param, &param_len, deadline)) {
#ifdef BOARD_BT_MFB_PIN
            mfb_hold(false);   /* it answered -- stop "pressing" MFB (see mfb_hold()) */
#endif
            if (event_code != BM64_EVENT_ACK) send_ack(event_code);
            if (event_code == BM64_EVENT_DEVICE_STATE && param_len >= 1) {
                ESP_LOGI(TAG, "startup device state: 0x%02X", param[0]);
                if (param[0] == BM64_STATE_POWER_ON || param[0] == 0x0F /*standby*/) {
                    ready = true;
                }
            }
        }
        if (!ready) {
            ESP_LOGW(TAG, "module did not reach standby within 3s -- continuing anyway");
        }
#ifdef BOARD_BT_MFB_PIN
        mfb_hold(false);   /* never leave it held, answer or not */
#endif
    }

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

    /* A stop requested during the startup sequence above (the config
     * autocheck fires the moment the BD address lands, which is mid-startup)
     * shouldn't have to wait out the rest of it. */
    if (s_task_stop) { s_task = NULL; vTaskDelete(NULL); }

    /* Also one-time at startup: set the module's own advertised Bluetooth
     * name to this board's name (e.g. "dualphone"), so pairing shows
     * something short and recognizable instead of a generic factory name.
     * Confirmed frame format 2026-09-22 straight from Microchip's real
     * command-set doc: BT_Device_Name is a fixed 32-byte field, no length
     * prefix, ASCII, unused tail padded with zeros. That same doc notes
     * this does NOT persist to the module's own EEPROM/power cycle -- which
     * is exactly why this runs here, every boot, rather than being a
     * one-time provisioning step. */
    {
        char name[32] = {0};
        board_name_get(name, sizeof(name));
        send_command(BM64_CHANGE_DEVICE_NAME_CMD, (const uint8_t *)name, sizeof(name));
        ESP_LOGI(TAG, "set BT device name to '%s'", name);
    }

    while (!s_task_stop) {
#ifdef BOARD_BT_MFB_PIN
        if (s_mfb_held && s_status.link_up) mfb_hold(false);
#endif
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

    /* Cooperative exit -- see stop_task(). Clearing the handle from inside
     * the task is what tells stop_task() it's safe to touch the UART. */
    s_task = NULL;
    vTaskDelete(NULL);
}

#ifdef BOARD_BT_MFB_PIN
/* MFB must not be held high indefinitely. The BM64 treats it as its power
 * button (EEPROM 0xAE bit0 = 0, "MFB used as power function"), and a press
 * that starts while the module is OFF and is held past reset_to_default_timer
 * (0x1AC) triggers a "MASTER RESET" that clears the paired-device list --
 * confirmed 2026-09-28 on newphone: every cold boot (and every ESP32 reset
 * while RST_N was bench-wired) pulsed MFB low->high and held it, and the
 * phone's pairing was gone every time, even right after the module had
 * reported saving its EEPROM at a clean power-off. MFB high is still
 * needed while the module is off (it's also UART RX_IND -- an off module
 * with MFB low ignores the UART entirely), so: raise it to wake/power the
 * module, drop it as soon as any valid frame comes back. With the module
 * on, the UART works fine with MFB low. */
static bool               s_mfb_ctrl;   /* this board drives MFB (assert_bm64_control_pins() ran) */
static volatile bool      s_mfb_high;
static esp_timer_handle_t s_mfb_timer;
static portMUX_TYPE       s_mfb_mux = portMUX_INITIALIZER_UNLOCKED;

static void mfb_timer_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mfb_mux);
    if (!s_mfb_held) {
        gpio_set_level(BOARD_BT_MFB_PIN, 0);
        s_mfb_high = false;
    }
    portEXIT_CRITICAL(&s_mfb_mux);
}

/* Held high = the power-on phase only (Fig 5-3/5-6). Releasing doesn't drop
 * the pin at once: it falls 150 ms later via the same timer the per-command
 * window uses, so an in-flight command still gets its slot. */
static void mfb_hold(bool on)
{
    if (!s_mfb_ctrl) return;
    if (on) {
        if (s_mfb_timer) esp_timer_stop(s_mfb_timer);
        portENTER_CRITICAL(&s_mfb_mux);
        s_mfb_held = true;
        s_mfb_high = true;
        gpio_set_level(BOARD_BT_MFB_PIN, 1);
        portEXIT_CRITICAL(&s_mfb_mux);
    } else if (s_mfb_held) {
        s_mfb_held = false;
        if (s_mfb_timer) {
            esp_timer_stop(s_mfb_timer);
            esp_timer_start_once(s_mfb_timer, 150 * 1000);
        }
        ESP_LOGI(TAG, "module answering -- MFB back to idle-low (held, it would factory-reset the pairing list)");
    }
}

/* Fig 5-4 "RX indication after power-on state": the module idles in 32 kHz
 * mode with MFB low. Wake it with a 1 ms pulse, 2 ms low, then MFB high
 * and 2 ms before the first byte; keep MFB high >= 150 ms (longer than the
 * UART command slot). Back-to-back commands just extend the window. */
static void mfb_before_tx(void)
{
    if (!s_mfb_ctrl || s_mfb_held) return;
    if (s_mfb_timer) esp_timer_stop(s_mfb_timer);
    if (!s_mfb_high) {
        gpio_set_level(BOARD_BT_MFB_PIN, 1);
        esp_rom_delay_us(1000);
        gpio_set_level(BOARD_BT_MFB_PIN, 0);
        esp_rom_delay_us(2000);
        gpio_set_level(BOARD_BT_MFB_PIN, 1);
        s_mfb_high = true;
        esp_rom_delay_us(2000);
    }
}

static void mfb_after_tx(void)
{
    if (!s_mfb_ctrl || s_mfb_held || !s_mfb_timer) return;
    esp_timer_start_once(s_mfb_timer, 150 * 1000);
}

/* Bring-up rig only: on a board that's bench-wired the BM64's MFB/RST_N/P2_0
 * to GPIO18-20 (see board_pins_smd4.h), those three must be driven high for
 * the module to be powered/out-of-reset/in-ROM-app-mode at all. GPIO18-20
 * don't survive a reset (revert to floating input), so this has to run
 * every time the UART is (re)brought up, not just once at boot. Gated the
 * same way bringup.c's bm64_pin command is: only safe to drive these pads
 * when this board's runtime config is single-line with no microSD
 * (ag1170/sdcard don't have them claimed). No-op (with a log line) on a
 * 2-line or has-microSD config, and compiled out entirely on boards whose
 * pin map doesn't define these macros (SMD2).
 *
 * Root cause of a full night of "MFB won't reliably hold high" debugging
 * that looked exactly like a wiring/connection fault (see
 * archive/BM64_MFB_GPIO_DEBUG.md for the full story): a bare
 * gpio_set_direction() + gpio_set_level() pair leaves pull_up_en at
 * whatever it last was, and a leftover internal pull-up was fighting the
 * output. Fixed by using full gpio_config() instead (pull-up/down and
 * interrupts explicitly disabled in the same atomic call as the mode) --
 * matches ag1170.c's config_line_gpio(), the proven pattern for this
 * project's other bench-configured GPIO outputs (the SLIC's FR/RM pins).
 * gpio_sleep_sel_dis()/gpio_hold_dis() below and the MFB low-then-high
 * pulse are cheap insurance from earlier (now-superseded) theories, not
 * the fix -- kept anyway since they're harmless and guard against a couple
 * of real edge cases (this pin's state getting disturbed by an actual
 * light-sleep transition; MFB needing a genuine wake edge per the
 * datasheet's "UART RX_IND" description of that pin). */
static void assert_bm64_control_pins(void)
{
    board_hw_config_t cfg = board_hw_config_get();
    /* RST_N/P2_0 live on the GPIO18-20 pads line 1's SLIC / the microSD slot
     * also use, so they're only safe on a single-line, no-microSD config.
     * MFB may or may not share that fate -- SMD5 gives it its own pin
     * (BOARD_BT_MFB_DEDICATED), and then it's always drivable. */
    bool shared_pads_free = (cfg.num_lines == 1 && !cfg.has_microsd);
#ifdef BOARD_BT_MFB_DEDICATED
    const bool mfb_free = true;
#else
    const bool mfb_free = shared_pads_free;
#endif
    if (!shared_pads_free) {
        ESP_LOGW(TAG, "not asserting BM64 RST_N/P2_0 (GPIO19-20)%s -- this board's config is "
                      "%d-line%s, those pads are claimed by line 1's SLIC/microSD",
                 mfb_free ? " (MFB has its own pin, still driven)" : " or MFB",
                 cfg.num_lines, cfg.has_microsd ? " with microSD" : "");
    }
    if (mfb_free) {
        gpio_hold_dis(BOARD_BT_MFB_PIN);
        gpio_config_t mfb_cfg = {
            .pin_bit_mask = 1ULL << BOARD_BT_MFB_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&mfb_cfg);
        gpio_sleep_sel_dis(BOARD_BT_MFB_PIN);
        /* MFB idles LOW (see mfb_hold()); poll_task's startup raises it for
         * the datasheet power-on sequence. */
        gpio_set_level(BOARD_BT_MFB_PIN, 0);
        s_mfb_high = false;
        s_mfb_held = false;
        if (!s_mfb_timer) {
            const esp_timer_create_args_t targs = { .callback = mfb_timer_cb, .name = "bm64_mfb" };
            esp_timer_create(&targs, &s_mfb_timer);
        }
        s_mfb_ctrl = true;
    }
    if (shared_pads_free) {
        gpio_hold_dis(diag_rst_pin());
        gpio_hold_dis(diag_p20_pin());
        gpio_config_t ctl_cfg = {
            .pin_bit_mask = (1ULL << diag_rst_pin()) | (1ULL << diag_p20_pin()),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&ctl_cfg);
        gpio_sleep_sel_dis(diag_rst_pin());
        gpio_sleep_sel_dis(diag_p20_pin());
        /* High = out of reset, normal (not ROM/IBDK) mode. */
        gpio_set_level(diag_rst_pin(), 1);
        gpio_set_level(diag_p20_pin(), 1);
    }
    if (mfb_free || shared_pads_free) vTaskDelay(pdMS_TO_TICKS(200));
}
#endif

/* Just the UART1 peripheral -- no pin control at all. Shared by ensure_uart()
 * (normal-mode path, which additionally forces MFB/RST_N/P2_0 for Flash
 * Application mode) and bt_companion_raw_send() (IBDK/test-mode probing,
 * which needs the OPPOSITE pin state -- P2_0 held low -- so it must never
 * go through the normal-mode pin assertion). */
static esp_err_t ensure_uart_peripheral_only(void)
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

static esp_err_t ensure_uart(void)
{
    if (s_uart_installed) return ESP_OK;
#ifdef BOARD_BT_MFB_PIN
    assert_bm64_control_pins();
#endif
    return ensure_uart_peripheral_only();
}

static void start_task(void)
{
    if (s_task) return; /* already running */
    if (ensure_uart() != ESP_OK) return;
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_lock);
    s_task_stop = false;
    xTaskCreate(poll_task, "bt_companion", 3072, NULL, tskIDLE_PRIORITY + 1, &s_task);
}

/* Asks poll_task to exit and waits for it, rather than vTaskDelete()ing it
 * from under whatever UART call it's in the middle of -- see s_task_stop.
 * Worst case is one startup-sequence's worth of wait (~4s); the timeout is
 * only a backstop so a genuinely wedged task can't block this forever. */
static void stop_task(void)
{
    if (s_task) {
        s_task_stop = true;
        for (int waited = 0; s_task && waited < 6000; waited += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_task) {
            ESP_LOGW(TAG, "poll task didn't exit in time -- deleting it outright; the "
                          "UART may need reinstalling if the next command misbehaves");
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
    portENTER_CRITICAL(&s_lock);
    bool had_call = (s_status.call_state != BT_CALL_IDLE);
    memset(&s_status, 0, sizeof(s_status));
    memset(s_dev_call, 0, sizeof(s_dev_call));
    memset(s_dev_cid, 0, sizeof(s_dev_cid));
    portEXIT_CRITICAL(&s_lock);
    /* Switched off (or parked for a config check) mid-call: tell the listener,
     * or a ringing bell would never be told to stop. */
    if (had_call && s_call_cb) s_call_cb(BT_CALL_IDLE, s_call_cb_ctx);
    /* UART left installed (cheap, and BOARD_UART_TX/RX have no other
     * claimant) so re-enabling doesn't need to reinstall the driver. */
}

/* ---- IBDK mode + EEPROM access ----------------------------------------- */

#define CFG_VERIFIED_NVS_KEY "cfg_bd"   /* BD address whose config we've verified */

static bool s_ibdk_ready;

#ifdef BOARD_BT_MFB_PIN

/* Finds the HCI ACL Data response (02 FF 0F ...) inside a raw RX capture.
 * The module interleaves Number_Of_Completed_Packets events with the actual
 * ACL reply, so the reply isn't reliably at offset 0. */
static int find_acl_response(const uint8_t *rx, int n)
{
    for (int i = 0; i + 5 <= n; i++) {
        if (rx[i] == 0x02 && rx[i + 1] == 0xFF && rx[i + 2] == 0x0F) return i;
    }
    return -1;
}

bool bt_companion_ibdk_ready(void) { return s_ibdk_ready; }

esp_err_t bt_companion_ibdk_enter(bool toggle_pins)
{
    /* Keyed on the task, not s_enabled, to match every other raw_* entry
     * point here -- that lets bt_companion_config_check() park the poll task
     * without touching the persisted enabled flag. */
    if (s_task) return ESP_ERR_INVALID_STATE; /* poll task would fight for RX */
    board_hw_config_t cfg = board_hw_config_get();
    if (cfg.num_lines != 1 || cfg.has_microsd) {
        ESP_LOGE(TAG, "IBDK: refusing -- GPIO18-20 are claimed by line 1 SLIC/microSD");
        return ESP_ERR_INVALID_STATE;
    }

    s_ibdk_ready = false;

    if (toggle_pins) {
        /* Sequence from the Arduino bridge sketch: P2_0 LOW + MFB LOW ->
         * RST LOW -> RST HIGH -> MFB HIGH, with generous settling between
         * each (the module needs it; manual testing had seconds of typing
         * delay between steps and that's what worked). */
        gpio_hold_dis(BOARD_BT_MFB_PIN);
        gpio_hold_dis(diag_rst_pin());
        gpio_hold_dis(diag_p20_pin());
        gpio_config_t gpio_cfg = {
            .pin_bit_mask = (1ULL << BOARD_BT_MFB_PIN) | (1ULL << diag_rst_pin()) |
                            (1ULL << diag_p20_pin()),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&gpio_cfg);
        gpio_sleep_sel_dis(BOARD_BT_MFB_PIN);
        gpio_sleep_sel_dis(diag_rst_pin());
        gpio_sleep_sel_dis(diag_p20_pin());

        ESP_LOGI(TAG, "IBDK: entering (P2_0 low across reset)");
        gpio_set_level(diag_p20_pin(), 0);
        gpio_set_level(BOARD_BT_MFB_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(diag_rst_pin(), 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(diag_rst_pin(), 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(BOARD_BT_MFB_PIN, 1);
        s_mfb_held = true; s_mfb_high = true;   /* poll_task's startup hands it back to idle-low */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    uint8_t rx[64];
    int n;

    /* 1. Vendor init (0xFC31). Retried -- the module sometimes needs extra
     *    boot time after the pin-toggle entry. */
    static const uint8_t init_fc31[] = { 0x01, 0x31, 0xFC, 0x0C,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
    n = 0;
    for (int attempt = 0; attempt < 3 && (n < 7 || rx[0] != 0x04); attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(500));
        n = bt_companion_raw_xfer(init_fc31, sizeof(init_fc31), rx, sizeof(rx), 2000);
    }
    if (n < 7 || rx[0] != 0x04 || rx[1] != 0x0E) {
        ESP_LOGE(TAG, "IBDK: vendor init (0xFC31) failed (%d bytes)", n);
        if (toggle_pins && !s_diag_tried_swap) {
            /* Maybe the two leads are on each other's pins. Try them swapped,
             * once: the worst a wrong guess does is pull RST_N low where P2_0
             * was meant (a reset) or toggle P2_0 (an input that doesn't matter
             * once running). */
            s_diag_tried_swap = true;
            int r0 = s_rst_pin, p0 = s_p20_pin;
            s_rst_pin = p0;
            s_p20_pin = r0;
            ESP_LOGW(TAG, "IBDK: trying the leads the other way round (RST_N on GPIO%d, P2_0 on GPIO%d)",
                     s_rst_pin, s_p20_pin);
            esp_err_t e = bt_companion_ibdk_enter(true);
            s_diag_tried_swap = false;
            if (e == ESP_OK) {
                ESP_LOGW(TAG, "IBDK: worked swapped -- the leads are crossed; remembering that");
                diag_pins_save();
            } else {
                s_rst_pin = r0;
                s_p20_pin = p0;
                ESP_LOGE(TAG, "IBDK: neither way worked. Check the leads reach the carrier's J2 and the "
                              "module has power. `bm64_findwire <gpio>` finds which of GPIO18-21 is RST_N; "
                              "`bm64_diagpins <rst> <p20>` tells the firmware (now RST_N=GPIO%d, P2_0=GPIO%d)",
                         s_rst_pin, s_p20_pin);
            }
            return e;
        }
        return ESP_FAIL;
    }

    /* 2-3. Buffer size negotiation. */
    static const uint8_t read_buf[] = { 0x01, 0x05, 0x10, 0x00 };
    n = bt_companion_raw_xfer(read_buf, sizeof(read_buf), rx, sizeof(rx), 2000);
    if (n < 7 || rx[0] != 0x04 || rx[1] != 0x0E) {
        ESP_LOGE(TAG, "IBDK: Read_Buffer_Size failed");
        return ESP_FAIL;
    }
    static const uint8_t host_buf[] = { 0x01, 0x33, 0x0C, 0x07,
        0x01, 0x00, 0x02, 0x03, 0x00, 0x04, 0x00 };
    n = bt_companion_raw_xfer(host_buf, sizeof(host_buf), rx, sizeof(rx), 2000);
    if (n < 7 || rx[0] != 0x04 || rx[1] != 0x0E) {
        ESP_LOGE(TAG, "IBDK: Host_Buffer_Size failed");
        return ESP_FAIL;
    }

    /* 4. Create_Connection -> establishes the 0x0FFF pseudo-handle every
     *    EEPROM ACL packet is addressed to. */
    static const uint8_t create_conn[] = { 0x01, 0x05, 0x04, 0x0D,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    n = bt_companion_raw_xfer(create_conn, sizeof(create_conn), rx, sizeof(rx), 5000);
    if (n < 18 || rx[0] != 0x04 || rx[1] != 0x0F) {
        ESP_LOGE(TAG, "IBDK: Create_Connection failed (%d bytes)", n);
        return ESP_FAIL;
    }

    /* 5-6. Memory-type / buffer-window setup. Replies aren't checked --
     *      isUpdate.exe's own capture doesn't act on them either. */
    static const uint8_t acl_setup1[] = { 0x02, 0xFF, 0x0F, 0x07, 0x00,
        0x00, 0x01, 0x03, 0x00, 0x05, 0x02, 0x00 };
    bt_companion_raw_xfer(acl_setup1, sizeof(acl_setup1), rx, sizeof(rx), 2000);
    static const uint8_t acl_setup2[] = { 0x02, 0xFF, 0x0F, 0x04, 0x00,
        0x20, 0x00, 0x00, 0x00 };
    bt_companion_raw_xfer(acl_setup2, sizeof(acl_setup2), rx, sizeof(rx), 2000);

    s_ibdk_ready = true;

    /* Probe a byte with a known value as proof the link really works, rather
     * than trusting the setup replies above. */
    uint8_t probe = 0;
    if (bt_companion_eeprom_read(0x0094, &probe, 1) != ESP_OK) {
        s_ibdk_ready = false;
        ESP_LOGE(TAG, "IBDK: probe read failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "IBDK: ready (probe 0x0094 = 0x%02X)", probe);
    return ESP_OK;
}

esp_err_t bt_companion_ibdk_exit(void)
{
    board_hw_config_t cfg = board_hw_config_get();
    if (cfg.num_lines != 1 || cfg.has_microsd) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "IBDK: exiting -> normal Flash Application mode");
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << BOARD_BT_MFB_PIN) | (1ULL << diag_rst_pin()) |
                        (1ULL << diag_p20_pin()),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);
    gpio_set_level(diag_p20_pin(), 1);   /* high across reset = normal mode */
    gpio_set_level(BOARD_BT_MFB_PIN, 0);
    gpio_set_level(diag_rst_pin(), 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(diag_rst_pin(), 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(BOARD_BT_MFB_PIN, 1);
    s_mfb_held = true; s_mfb_high = true;   /* poll_task's startup hands it back to idle-low */
    vTaskDelay(pdMS_TO_TICKS(500));
    s_ibdk_ready = false;
    return ESP_OK;
}

esp_err_t bt_companion_eeprom_read(uint16_t addr, uint8_t *out, size_t len)
{
    if (!out || len == 0 || len > 256) return ESP_ERR_INVALID_ARG;
    if (!s_ibdk_ready) return ESP_ERR_INVALID_STATE;

    /* ACL EEPROM READ: 02 FF 0F [len16] 10 01 | 0A 00 05 02 | addr32 | len32 */
    uint8_t cmd[19] = {
        0x02, 0xFF, 0x0F, 0x0E, 0x00,
        0x10, 0x01,
        0x0A, 0x00, 0x05, 0x02,
        (uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), 0x00, 0x00,
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF), 0x00, 0x00
    };
    uint8_t rx[320];
    int n = bt_companion_raw_xfer(cmd, sizeof(cmd), rx, sizeof(rx), 2000);
    int acl = find_acl_response(rx, n);
    if (acl < 0 || acl + 5 + 10 > n) return ESP_ERR_TIMEOUT;
    int payload = acl + 5;
    if (rx[payload] != 0x10 || rx[payload + 1] != 0x01) return ESP_FAIL;
    int data_start = payload + 10;  /* skip type(2) + payload_len(4) + count(4) */
    if (data_start + (int)len > n) return ESP_ERR_INVALID_SIZE;
    memcpy(out, &rx[data_start], len);
    return ESP_OK;
}

esp_err_t bt_companion_eeprom_write(uint16_t addr, uint8_t value)
{
    if (!s_ibdk_ready) return ESP_ERR_INVALID_STATE;

    /* Same shape as the read, sub-command 11 01 / type 0B, one data byte. */
    uint8_t cmd[20] = {
        0x02, 0xFF, 0x0F, 0x0F, 0x00,
        0x11, 0x01,
        0x0B, 0x00, 0x05, 0x02,
        (uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        value
    };
    uint8_t rx[320];
    int n = bt_companion_raw_xfer(cmd, sizeof(cmd), rx, sizeof(rx), 2000);
    int acl = find_acl_response(rx, n);
    if (acl < 0 || acl + 5 + 2 > n) return ESP_ERR_TIMEOUT;
    int payload = acl + 5;
    if (rx[payload] != 0x11 || rx[payload + 1] != 0x01) return ESP_FAIL;
    return ESP_OK;
}

#else  /* no GPIO control of the mode pins -> IBDK is unreachable */

bool      bt_companion_ibdk_ready(void) { return false; }
esp_err_t bt_companion_ibdk_enter(bool toggle_pins)
{
    (void)toggle_pins; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t bt_companion_ibdk_exit(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bt_companion_diag_pins_set(int rst_gpio, int p20_gpio)
{
    (void)rst_gpio; (void)p20_gpio; return ESP_ERR_NOT_SUPPORTED;
}
void bt_companion_diag_pins_get(int *rst_gpio, int *p20_gpio)
{
    if (rst_gpio) *rst_gpio = -1;
    if (p20_gpio) *p20_gpio = -1;
}
esp_err_t bt_companion_eeprom_read(uint16_t addr, uint8_t *out, size_t len)
{
    (void)addr; (void)out; (void)len; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t bt_companion_eeprom_write(uint16_t addr, uint8_t value)
{
    (void)addr; (void)value; return ESP_ERR_NOT_SUPPORTED;
}

#endif /* BOARD_BT_MFB_PIN */

/* ---- Known-good EEPROM configuration ----------------------------------- */

/* Why each byte, and what the stock value breaks -- full derivation in
 * Bluetooth/BM64_AUDIO_DEBUG.md. The short version: a stock BM64CLS1 table routes
 * A2DP audio to I2S for the EVB's external Yamaha DSP amp, so the analog
 * AOHPL pin this board taps stays silent no matter how loud the phone is. */
static const struct { uint16_t addr; uint8_t expected; const char *what; }
s_expected_config[] = {
    { 0x01FC, 0x06, "audio to internal codec (analog AOHPL), not I2S" },
    { 0x0094, 0x13, "sample-rate converter off (I2S-only feature)" },
    { 0x0171, 0x40, "voice/tone stereo off, matching the SPKOUT table" },
    { 0x01D7, 0x94, "stereo DAC path single-ended" },
    { 0x01D8, 0x93, "mono DAC path single-ended" },
    { 0x0348, 0x0A, "single-ended with earphone (not capless)" },
};
#define N_EXPECTED_CONFIG (sizeof(s_expected_config) / sizeof(s_expected_config[0]))
_Static_assert(N_EXPECTED_CONFIG <= BM64_CONFIG_ITEM_MAX, "grow BM64_CONFIG_ITEM_MAX");

static bm64_config_report_t s_last_report;
static bool                 s_have_last_report;
/* Serializes config checks. Without this, the boot autocheck and a
 * console/web-UI-triggered check will both drive IBDK mode and the same UART
 * at once -- confirmed live 2026-09-26, which looked like a dead module
 * ("no BD address reply") rather than the race it actually was. */
static bool                 s_config_busy;

static void config_verified_addr_get(char *out, size_t out_len)
{
    if (out_len) out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(BT_COMPANION_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = out_len;
    if (nvs_get_str(h, CFG_VERIFIED_NVS_KEY, out, &len) != ESP_OK && out_len) out[0] = '\0';
    nvs_close(h);
}

static void config_verified_addr_set(const char *addr)
{
    nvs_handle_t h;
    if (nvs_open(BT_COMPANION_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, CFG_VERIFIED_NVS_KEY, addr) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

bool bt_companion_config_is_verified(void)
{
    if (module() == BOARD_BT_BT401) return false;   /* nothing to verify */
    bt_companion_status_t st = bt_companion_get_status();
    if (!st.bd_addr[0]) return false;  /* don't know which module this is yet */
    char saved[18] = {0};
    config_verified_addr_get(saved, sizeof(saved));
    return saved[0] && strcmp(saved, st.bd_addr) == 0;
}

bool bt_companion_config_last_report(bm64_config_report_t *out)
{
    if (!s_have_last_report) return false;
    if (out) *out = s_last_report;
    return true;
}

esp_err_t bt_companion_config_check(bool apply, bm64_config_report_t *out)
{
    if (module() == BOARD_BT_BT401) {
        (void)apply;
        if (out) {
            memset(out, 0, sizeof(*out));
            snprintf(out->error, sizeof(out->error), "the BT401 has no EEPROM configuration step");
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
#ifndef BOARD_BT_MFB_PIN
    (void)apply;
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->error, sizeof(out->error),
                 "this board has no GPIO control of the BM64 mode pins");
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_lock);
    bool busy = s_config_busy;
    if (!busy) s_config_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        ESP_LOGW(TAG, "config check already running -- ignoring this request");
        if (out) {
            memset(out, 0, sizeof(*out));
            snprintf(out->error, sizeof(out->error), "a configuration check is already running");
        }
        return ESP_ERR_INVALID_STATE;
    }

    bm64_config_report_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.n_items = (int)N_EXPECTED_CONFIG;
    for (size_t i = 0; i < N_EXPECTED_CONFIG; i++) {
        rep.items[i].addr     = s_expected_config[i].addr;
        rep.items[i].expected = s_expected_config[i].expected;
        rep.items[i].what     = s_expected_config[i].what;
    }

    /* Park the poll task rather than calling bt_companion_set_enabled(false):
     * that one persists the flag to NVS, so a reset or crash midway through
     * this (IBDK mode involves two module resets) would leave Bluetooth
     * switched off from the user's point of view. stop_task()/start_task()
     * leave both s_enabled and NVS alone. */
    bool had_task = (s_task != NULL);

    /* The BD address identifies which module we're about to verify, so it has
     * to be captured before IBDK mode takes the module away. Prefer the copy
     * the poll task already queried at startup -- re-asking costs a round trip
     * and, worse, has to happen with the task stopped, which is exactly when
     * a stray reply gets missed. Only query directly if the poller never got
     * one (e.g. a check triggered before the link settled). */
    bt_companion_status_t pre = bt_companion_get_status();
    if (pre.bd_addr[0]) {
        snprintf(rep.bd_addr, sizeof(rep.bd_addr), "%s", pre.bd_addr);
    }
    if (had_task) stop_task();
    if (!rep.bd_addr[0] &&
        bt_companion_read_bd_addr(2000, rep.bd_addr, sizeof(rep.bd_addr)) != ESP_OK) {
        rep.bd_addr[0] = '\0';
        ESP_LOGW(TAG, "config check: couldn't read BD address -- proceeding, but the "
                      "result won't be remembered for this module");
    }

    esp_err_t err = bt_companion_ibdk_enter(true);
    if (err != ESP_OK) {
        snprintf(rep.error, sizeof(rep.error), "couldn't enter IBDK mode (%s)",
                 esp_err_to_name(err));
        goto done;
    }

    for (size_t i = 0; i < N_EXPECTED_CONFIG; i++) {
        bm64_config_item_t *it = &rep.items[i];
        uint8_t got = 0;
        if (bt_companion_eeprom_read(it->addr, &got, 1) != ESP_OK) {
            snprintf(rep.error, sizeof(rep.error), "EEPROM read failed at 0x%04X", it->addr);
            goto exit_ibdk;
        }
        it->found   = got;
        it->matched = (got == it->expected);
        if (!it->matched) rep.n_mismatched++;

        if (!it->matched && apply) {
            if (bt_companion_eeprom_write(it->addr, it->expected) != ESP_OK) {
                snprintf(rep.error, sizeof(rep.error), "EEPROM write failed at 0x%04X", it->addr);
                goto exit_ibdk;
            }
            /* Verify rather than trust the write's own ACK. */
            if (bt_companion_eeprom_read(it->addr, &got, 1) != ESP_OK || got != it->expected) {
                snprintf(rep.error, sizeof(rep.error),
                         "wrote 0x%04X but read back 0x%02X", it->addr, got);
                goto exit_ibdk;
            }
            it->found   = got;
            it->written = true;
            rep.n_written++;
        }
    }

    /* ok = nothing left differing. In check-only mode that means it was
     * already correct; in apply mode, that every write verified. */
    rep.ok = true;
    for (size_t i = 0; i < N_EXPECTED_CONFIG; i++) {
        if (rep.items[i].found != rep.items[i].expected) rep.ok = false;
    }

exit_ibdk:
    bt_companion_ibdk_exit();
done:
    if (rep.ok && rep.bd_addr[0]) config_verified_addr_set(rep.bd_addr);
    if (had_task) start_task();

    ESP_LOGI(TAG, "config check: %d/%d correct, %d written%s%s",
             rep.n_items - rep.n_mismatched + rep.n_written, rep.n_items,
             rep.n_written, rep.error[0] ? ", error: " : "", rep.error);

    s_last_report = rep;
    s_have_last_report = true;
    portENTER_CRITICAL(&s_lock);
    s_config_busy = false;
    portEXIT_CRITICAL(&s_lock);
    if (out) *out = rep;
    return rep.error[0] ? ESP_FAIL : ESP_OK;
#endif
}

/* One-shot: waits for the link to come up far enough to know which module
 * this is, then repairs its config if we've never verified that BD address.
 * A separate task because the check stops and restarts the poll task, which
 * it obviously can't do from inside the poll task itself. */
static void config_autocheck_task(void *arg)
{
    (void)arg;
    const int poll_ms = 500, max_wait_ms = 30000;
    for (int waited = 0; waited < max_wait_ms; waited += poll_ms) {
        if (!s_enabled) goto out;   /* turned off again while we waited */
        bt_companion_status_t st = bt_companion_get_status();
        if (st.link_up && st.bd_addr[0]) {
            if (bt_companion_config_is_verified()) {
                ESP_LOGI(TAG, "EEPROM config already verified for %s -- skipping check",
                         st.bd_addr);
            } else {
                ESP_LOGI(TAG, "unverified BM64 (%s) -- checking/repairing EEPROM config; "
                              "Bluetooth will be down for ~15s", st.bd_addr);
                bt_companion_config_check(true, NULL);
            }
            goto out;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
    ESP_LOGW(TAG, "config autocheck: module never reported a BD address -- skipping "
                  "(run it by hand from Settings -> Bluetooth)");
out:
    s_autocheck_task = NULL;
    vTaskDelete(NULL);
}

static void start_autocheck(void)
{
    if (s_autocheck_task) return;  /* already waiting */
    /* Needs its own stack rather than piggybacking on the caller: it sleeps
     * for up to 30s waiting for the link, then runs a ~15s IBDK sequence. */
    xTaskCreate(config_autocheck_task, "bm64_cfgchk", 4096, NULL,
                tskIDLE_PRIORITY + 1, &s_autocheck_task);
}

void bt_companion_init(void)
{
    load_config();
    if (s_enabled && !board_hw_config_get().has_bt_module) {
        ESP_LOGW(TAG, "enabled in settings but no Bluetooth module is marked as fitted "
                      "(Settings -> Hardware) -- not starting the link");
        return;
    }
    if (module() == BOARD_BT_BT401 && board_hw_config_get().has_bt_module) {
        nvs_handle_t h;
        if (nvs_open(BT_COMPANION_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(h, "sleep_h", &v) == ESP_OK && v <= 24) s_sleep_hours = v;
            nvs_close(h);
        }
        xTaskCreate(sleep_timer_task, "bt_sleep", 2560, NULL, tskIDLE_PRIORITY + 1, &s_sleep_timer_task);
        if (s_enabled) bt401_start(bt401_notify);
        else bt401_sleep();     /* switched off: keep the radio off after a power cycle too */
        return;
    }
    if (s_enabled) {
        start_task();
        start_autocheck();
    }
}
