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
#define BT_LINE_MAX             96

/* Line-based ASCII protocol between this component and the companion
 * board's own firmware (doesn't exist yet -- see bt_companion.h). Kept
 * deliberately tiny and human-readable (easy to poke at by hand over a
 * second UART bridge while bringing the companion up):
 *
 *   C6 -> companion:  "STATUS?\n"   -- poll for current state
 *                      "PAIR\n"     -- enter pairing mode
 *   companion -> C6:   "STATUS CONNECTED <name>\n"
 *                       "STATUS DISCONNECTED\n"
 *
 * Anything else received is logged and ignored rather than treated as an
 * error -- future companion firmware versions may add lines this build
 * doesn't know about yet (e.g. RING), and an unrecognized line here
 * shouldn't be fatal. */
#define POLL_INTERVAL_MS   2000
#define LINK_TIMEOUT_MS    5000  /* no valid reply this long -> link_up = false */

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

/* Sends this board's own name so the ESP32 companion can broadcast it as
 * its Bluetooth device name -- see bt_uart_link.c's "NAME " handling on the
 * other end. Keeps the companion firmware identical across every C6 board
 * (SMD2/SMD4/THT, whatever name each one's been given) instead of needing a
 * per-board compile just to change the string a phone sees when pairing. */
static void send_name(void)
{
    char name[32];
    board_name_get(name, sizeof(name));
    char cmd[40];
    int n = snprintf(cmd, sizeof(cmd), "NAME %s\n", name);
    if (n > 0) uart_write_bytes(BT_COMPANION_UART, cmd, (size_t)n);
}

esp_err_t bt_companion_start_pairing(void)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    send_name(); /* re-sync in case the board name changed since boot */
    const char cmd[] = "PAIR\n";
    uart_write_bytes(BT_COMPANION_UART, cmd, sizeof(cmd) - 1);
    return ESP_OK;
}

esp_err_t bt_companion_test_tone(int hz, int ms)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    char cmd[32];
    int n = snprintf(cmd, sizeof(cmd), "TONE %d %d\n", hz, ms);
    if (n > 0) uart_write_bytes(BT_COMPANION_UART, cmd, (size_t)n);
    return ESP_OK;
}

esp_err_t bt_companion_test_level(bool high, int ms)
{
    if (!s_enabled || !s_uart_installed) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool up = s_status.link_up;
    portEXIT_CRITICAL(&s_lock);
    if (!up) return ESP_ERR_INVALID_STATE;
    char cmd[32];
    int n = snprintf(cmd, sizeof(cmd), "LEVEL %d %d\n", high ? 1 : 0, ms);
    if (n > 0) uart_write_bytes(BT_COMPANION_UART, cmd, (size_t)n);
    return ESP_OK;
}

/* Parses one already-NUL-terminated, newline-stripped line from the
 * companion. Anything not matching a known "STATUS ..." form is logged and
 * dropped -- see this file's top-of-file protocol comment. */
static void handle_line(const char *line)
{
    bool matched = false;
    portENTER_CRITICAL(&s_lock);
    if (strncmp(line, "STATUS CONNECTED ", 17) == 0) {
        s_status.bt_connected = true;
        strncpy(s_status.paired_name, line + 17, sizeof(s_status.paired_name) - 1);
        s_status.paired_name[sizeof(s_status.paired_name) - 1] = '\0';
        matched = true;
    } else if (strcmp(line, "STATUS DISCONNECTED") == 0) {
        s_status.bt_connected = false;
        s_status.paired_name[0] = '\0';
        matched = true;
    }
    if (matched) {
        s_status.link_up = true;
        s_last_reply_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_lock);
    if (!matched) ESP_LOGD(TAG, "unrecognized line from companion: '%s'", line);
}

static void poll_task(void *arg)
{
    char line[BT_LINE_MAX];
    size_t line_len = 0;
    const TickType_t period = pdMS_TO_TICKS(POLL_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();

    send_name(); /* sync once as soon as the link starts, before the first poll */

    while (1) {
        const char cmd[] = "STATUS?\n";
        uart_write_bytes(BT_COMPANION_UART, cmd, sizeof(cmd) - 1);

        /* Drain whatever's arrived since the last poll, splitting on '\n'.
         * Short read timeout per byte-chunk, not a long blocking read --
         * this task also owns sending the next poll, so it can't afford to
         * block for the full POLL_INTERVAL_MS waiting on a reply that may
         * never come (no companion connected yet, most of the time). */
        int64_t deadline_us = esp_timer_get_time() + 300000; /* 300ms read window per poll */
        while (esp_timer_get_time() < deadline_us) {
            uint8_t byte;
            int n = uart_read_bytes(BT_COMPANION_UART, &byte, 1, pdMS_TO_TICKS(50));
            if (n <= 0) continue;
            if (byte == '\n' || byte == '\r') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    handle_line(line);
                    line_len = 0;
                }
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = (char)byte;
            }
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

static void start_task(void)
{
    if (s_task) return; /* already running */
    if (!s_uart_installed) {
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
            return;
        }
        s_uart_installed = true;
    }
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
