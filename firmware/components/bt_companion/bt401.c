/* bt401.c — BT401 backend for bt_companion. See bt401.h.
 *
 * The module's behaviour comes from its datasheet and DFRobot's Arduino
 * library, checked on a real module; what is still untested is listed in
 * archive/hardware/bt401.md. */
#include "bt401.h"

#include "board.h"
#include "board_pins.h"
#include "bt401_proto.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "bt401";

#define BT_UART           UART_NUM_1
#define UART_RX_BUF       256
#define POLL_MS           1500   /* status query period (the module also volunteers changes) */
#define LINK_TIMEOUT_MS   6000   /* no line this long -> link_up = false */
#define BOOT_SETTLE_MS    1500   /* let the module finish booting before the first query */
#define RESET_WAIT_MS     3000   /* AT+CZ until the module answers again (burst ends ~2 s in) */

static portMUX_TYPE     s_lock = portMUX_INITIALIZER_UNLOCKED;
static bt401_state_t    s_st;
static bt401_parser_t   s_parser;
static int64_t          s_last_line_us;
static TaskHandle_t     s_task;
static volatile bool    s_stop;
static bool             s_uart_ready;
static bt401_notify_cb  s_notify;
static bool             s_module_restarted; /* power-up burst seen; task-only */
static bool             s_wake_needed;      /* QM said not Bluetooth mode; task-only */
static TaskHandle_t     s_sleep_task;

static int64_t now_us(void) { return esp_timer_get_time(); }

/* ---- UART ---------------------------------------------------------------- */

static esp_err_t uart_ready(void)
{
    if (s_uart_ready) return ESP_OK;
    uart_config_t cfg = {
        .baud_rate = BT401_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = ESP_OK;
    if (!uart_is_driver_installed(BT_UART)) {
        err = uart_driver_install(BT_UART, UART_RX_BUF, 0, 0, NULL, 0);
    }
    if (err == ESP_OK) err = uart_param_config(BT_UART, &cfg);   /* also fixes the baud rate */
    if (err == ESP_OK) {
        err = uart_set_pin(BT_UART, BOARD_UART_TX, BOARD_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_uart_ready = true;
    return ESP_OK;
}

static esp_err_t send_line(const char *cmd, const char *param)
{
    char line[48];
    size_t n = bt401_build_cmd(cmd, param, line, sizeof(line));
    if (!n || !s_uart_ready) return ESP_ERR_INVALID_STATE;
    uart_write_bytes(BT_UART, line, n);
    return ESP_OK;
}

/* ---- state -> bt_companion types ----------------------------------------- */

static bt_call_state_t map_call(bt401_call_t c)
{
    switch (c) {
    case BT401_CALL_INCOMING: return BT_CALL_INCOMING;
    case BT401_CALL_OUTGOING: return BT_CALL_OUTGOING;
    case BT401_CALL_ACTIVE:   return BT_CALL_ACTIVE;
    default:                  return BT_CALL_IDLE;
    }
}

void bt401_get_status(bt_companion_status_t *out)
{
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    int64_t age_ms = s_last_line_us ? (now_us() - s_last_line_us) / 1000 : INT64_MAX;
    out->link_up = s_task && age_ms < LINK_TIMEOUT_MS;
    out->bt_connected = out->link_up && s_st.connected;
    out->call_state = out->link_up ? map_call(s_st.call) : BT_CALL_IDLE;
    strlcpy(out->caller_id, out->link_up ? s_st.caller_id : "", sizeof(out->caller_id));
    strlcpy(out->bd_addr, s_st.addr, sizeof(out->bd_addr));
    /* The module doesn't say which phone is connected. */
    portEXIT_CRITICAL(&s_lock);
}

/* ---- task ---------------------------------------------------------------- */

static void handle_msg(const bt401_msg_t *m, bt_call_state_t *last_call, char *last_caller)
{
    char desc[80];
    bt401_describe(m, desc, sizeof(desc));

    portENTER_CRITICAL(&s_lock);
    unsigned chg = bt401_state_apply(&s_st, m);
    s_last_line_us = now_us();
    bool connected = s_st.connected;
    bt_call_state_t call = map_call(s_st.call);
    char caller[BT401_NUMBER_MAX];
    strlcpy(caller, s_st.caller_id, sizeof(caller));
    portEXIT_CRITICAL(&s_lock);

    if ((chg & BT401_CHG_LINK) && connected) board_led_set_state(BOARD_LED_BT_CONNECTED);

    /* "TR+0123456789" closes the burst the module sends when it powers up
     * (plugged in while the board runs) or resets (AT+CZ). */
    if (m->kind == BT401_MSG_OTHER && !strcmp(m->code, "TR")) s_module_restarted = true;
    if (m->kind == BT401_MSG_MODE && m->num != 1) s_wake_needed = true;

    /* The status poll and the repeated caller number would fill the log;
     * only lines that changed something (or aren't plain replies) go at INFO. */
    if (chg || m->kind == BT401_MSG_ERROR || m->kind == BT401_MSG_OTHER) {
        ESP_LOGI(TAG, "rx: %s", desc);
    } else {
        ESP_LOGD(TAG, "rx: %s", desc);
    }

    bool incoming_caller_changed = call == BT_CALL_INCOMING && strcmp(caller, last_caller) != 0;
    if (call != *last_call || incoming_caller_changed) {
        *last_call = call;
        strlcpy(last_caller, caller, BT401_NUMBER_MAX);
        if (s_notify) s_notify(call);
    }
}

static void bt401_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    bt_call_state_t last_call = BT_CALL_IDLE;
    char last_caller[BT401_NUMBER_MAX] = "";
    int64_t next_poll = now_us() + (int64_t)BOOT_SETTLE_MS * 1000;
    int stage = 0;             /* startup commands, one per tick so replies don't pile up */
    bool dac_on = true;        /* the module powers up with its DAC on */
    bool woken = false;        /* reset out of idle mode already (once per start) */
    bool mode_bt_due = false;  /* send CM01 once the reset's burst is in */

    ESP_LOGI(TAG, "listening at %d baud on GPIO%d/GPIO%d", BT401_BAUD, (int)BOARD_UART_TX, (int)BOARD_UART_RX);

    while (!s_stop) {
        int n = uart_read_bytes(BT_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            bt401_msg_t m;
            if (bt401_parser_feed(&s_parser, buf[i], &m)) handle_msg(&m, &last_call, last_caller);
        }

        if (s_module_restarted) {
            s_module_restarted = false;
            dac_on = true;     /* its DAC is back on after a power-up or reset */
            if (mode_bt_due) {
                mode_bt_due = false;
                send_line(BT401_CMD_MODE_BT, NULL);
            }
        }

        /* Left in idle mode by bt401_sleep(): the CG01 sent at startup only
         * applies after a reset, then Bluetooth mode. */
        if (s_wake_needed) {
            s_wake_needed = false;
            if (!woken) {
                woken = true;
                mode_bt_due = true;
                ESP_LOGI(TAG, "module in idle mode -- resetting it into Bluetooth mode");
                send_line(BT401_CMD_RESET, NULL);
                next_poll = now_us() + (int64_t)RESET_WAIT_MS * 1000;
                continue;
            }
        }

        /* The module's DAC output sits on the line's earpiece input through the
         * adapter's series resistor and, while on, loads the codec's own audio
         * (dial tone, prompts) even when silent. So it is only on while there is
         * Bluetooth audio: music or a call. */
        portENTER_CRITICAL(&s_lock);
        bool want_dac = s_st.music || s_st.call != BT401_CALL_IDLE;
        portEXIT_CRITICAL(&s_lock);
        if (want_dac != dac_on && (want_dac || stage >= 4)) {
            send_line(BT401_CMD_DAC, want_dac ? "01" : "00");
            dac_on = want_dac;
            ESP_LOGI(TAG, "DAC %s", want_dac ? "on (audio)" : "off (idle)");
        }

        int64_t now = now_us();
        if (now < next_poll) continue;
        next_poll = now + (int64_t)POLL_MS * 1000;

        static const char *const startup[] = { BT401_CMD_BG_ON, BT401_CMD_QUERY_NAME, BT401_CMD_QUERY_ADDR,
                                               BT401_CMD_CALLS_ON, BT401_CMD_QUERY_MODE };
        if (stage < (int)(sizeof(startup) / sizeof(startup[0]))) {
            send_line(startup[stage++], NULL);
            continue;
        }

        /* The module volunteers status changes, but asking is how a missed
         * one (or a link that dropped silently) is noticed. */
        send_line(BT401_CMD_QUERY_STATUS, NULL);

        /* While a call is ringing or live the number is repeated by the
         * module; ask once if it hasn't arrived. */
        portENTER_CRITICAL(&s_lock);
        bool want_number = s_st.call != BT401_CALL_IDLE && !s_st.caller_id[0];
        portEXIT_CRITICAL(&s_lock);
        if (want_number) send_line(BT401_CMD_QUERY_NUMBER, NULL);
    }

    /* Told the listener nothing while stopping: bt401_stop() does that. */
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t bt401_start(bt401_notify_cb cb)
{
    if (s_task) return ESP_OK;
    for (int waited = 0; s_sleep_task && waited < 6000; waited += 50) vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t err = uart_ready();
    if (err != ESP_OK) return err;
    uart_flush_input(BT_UART);
    s_notify = cb;
    portENTER_CRITICAL(&s_lock);
    bt401_state_init(&s_st);
    s_last_line_us = 0;
    portEXIT_CRITICAL(&s_lock);
    bt401_parser_init(&s_parser);
    s_stop = false;
    s_module_restarted = false;
    s_wake_needed = false;
    if (xTaskCreate(bt401_task, "bt401", 3072, NULL, tskIDLE_PRIORITY + 1, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void bt401_stop(bool disconnect)
{
    if (!s_task) return;
    if (disconnect) {
        send_line(BT401_CMD_DISCONNECT, NULL);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    send_line(BT401_CMD_DAC, "01");   /* leave the module's DAC as it powers up */
    s_stop = true;
    for (int waited = 0; s_task && waited < 1000; waited += 50) vTaskDelay(pdMS_TO_TICKS(50));
    bool had_call;
    portENTER_CRITICAL(&s_lock);
    had_call = s_st.call != BT401_CALL_IDLE;
    bt401_state_init(&s_st);
    s_last_line_us = 0;
    portEXIT_CRITICAL(&s_lock);
    if (had_call && s_notify) s_notify(BT_CALL_IDLE);  /* don't leave a bell ringing */
}

bool bt401_running(void) { return s_task != NULL; }

/* Idle mode alone keeps the radio up (32 mA, still discoverable); with
 * background Bluetooth off as well it really stops (7 mA at 3.3 V). CG only
 * applies after a reset, so: CG00, reset, then idle mode. bt401_start()
 * undoes it. */
static void sleep_task(void *arg)
{
    (void)arg;
    send_line(BT401_CMD_BG_OFF, NULL);
    vTaskDelay(pdMS_TO_TICKS(200));
    send_line(BT401_CMD_RESET, NULL);
    vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS));
    send_line(BT401_CMD_DAC, "00");           /* nothing to play; keep line 0 unloaded */
    vTaskDelay(pdMS_TO_TICKS(200));
    send_line(BT401_CMD_MODE_IDLE, NULL);
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_flush_input(BT_UART);
    ESP_LOGI(TAG, "module in idle mode, radio off");
    s_sleep_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t bt401_sleep(void)
{
    if (s_task || s_sleep_task) return ESP_ERR_INVALID_STATE;
    esp_err_t err = uart_ready();
    if (err != ESP_OK) return err;
    if (xTaskCreate(sleep_task, "bt401_sleep", 2560, NULL, tskIDLE_PRIORITY + 1, &s_sleep_task) != pdPASS) {
        s_sleep_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- call control -------------------------------------------------------- */

static esp_err_t command_if_linked(const char *cmd, bool need_phone)
{
    bt_companion_status_t st;
    bt401_get_status(&st);
    if (!st.link_up || (need_phone && !st.bt_connected)) return ESP_ERR_INVALID_STATE;
    return send_line(cmd, NULL);
}

static bt401_call_t current_call(void)
{
    portENTER_CRITICAL(&s_lock);
    bt401_call_t c = s_st.call;
    portEXIT_CRITICAL(&s_lock);
    return c;
}

esp_err_t bt401_accept_call(void)
{
    return current_call() == BT401_CALL_INCOMING ? command_if_linked(BT401_CMD_ANSWER, true) : ESP_ERR_NOT_FOUND;
}

esp_err_t bt401_reject_call(void)
{
    return current_call() == BT401_CALL_INCOMING ? command_if_linked(BT401_CMD_REJECT, true) : ESP_ERR_NOT_FOUND;
}

esp_err_t bt401_end_call(void)
{
    return bt401_call_live() ? command_if_linked(BT401_CMD_HANGUP, true) : ESP_ERR_NOT_FOUND;
}

esp_err_t bt401_start_pairing(void)
{
    return command_if_linked(BT401_CMD_DISCOVERABLE, false);
}

esp_err_t bt401_dial(const char *number)
{
    char line[48];
    if (!bt401_build_dial(number, line, sizeof(line))) return ESP_ERR_INVALID_ARG;
    bt_companion_status_t st;
    bt401_get_status(&st);
    if (!st.link_up || !st.bt_connected || current_call() != BT401_CALL_IDLE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    s_st.dialing = true;       /* so the TS+03 that follows reads as outgoing */
    portEXIT_CRITICAL(&s_lock);
    uart_write_bytes(BT_UART, line, strlen(line));
    return ESP_OK;
}

bool bt401_call_live(void)
{
    bt401_call_t c = current_call();
    return c == BT401_CALL_OUTGOING || c == BT401_CALL_ACTIVE;
}

bool bt401_music_playing(void)
{
    portENTER_CRITICAL(&s_lock);
    bool m = s_st.music;
    portEXIT_CRITICAL(&s_lock);
    return m;
}

/* ---- bring-up ------------------------------------------------------------ */

static void print_line_msgs(bt401_parser_t *p, const uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        bt401_msg_t m;
        if (bt401_parser_feed(p, buf[i], &m)) {
            char d[80];
            bt401_describe(&m, d, sizeof(d));
            printf("  <- %s\n", d);
        }
    }
}

static void listen_print(int ms)
{
    bt401_parser_t p;
    bt401_parser_init(&p);
    uint8_t buf[64];
    int64_t end = now_us() + (int64_t)ms * 1000;
    int total = 0;
    while (now_us() < end) {
        int n = uart_read_bytes(BT_UART, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) { total += n; print_line_msgs(&p, buf, n); }
    }
    printf("  (%d byte(s), %u line(s) in %d ms)\n", total, (unsigned)p.lines, ms);
}

esp_err_t bt401_send_at(const char *cmd, int listen_ms)
{
    if (!cmd || !*cmd) return ESP_ERR_INVALID_ARG;
    if (s_task) {
        esp_err_t e = send_line(cmd, NULL);
        printf("sent AT+%s (%s); the reply shows in the log, and bt401_state\n", cmd, esp_err_to_name(e));
        return e;
    }
    esp_err_t err = uart_ready();
    if (err != ESP_OK) return err;
    uart_flush_input(BT_UART);
    send_line(cmd, NULL);
    printf("sent AT+%s, listening %d ms:\n", cmd, listen_ms);
    listen_print(listen_ms);
    return ESP_OK;
}

esp_err_t bt401_probe(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    esp_err_t err = uart_ready();
    if (err != ESP_OK) return err;
    printf("BT401 probe at %d baud on GPIO%d (TX) / GPIO%d (RX)\n", BT401_BAUD, (int)BOARD_UART_TX, (int)BOARD_UART_RX);
    printf("listening 2 s for power-up lines (QA+, QM+):\n");
    listen_print(2000);
    static const char *const q[] = { BT401_CMD_QUERY_STATUS, BT401_CMD_QUERY_NAME, BT401_CMD_QUERY_ADDR };
    for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        printf("AT+%s:\n", q[i]);
        uart_flush_input(BT_UART);
        send_line(q[i], NULL);
        listen_print(500);
    }
    printf("If every listen shows 0 bytes: check power, TX/RX not crossed, and the baud rate "
           "(AT+CT05 changes it; factory default is 115200).\n");
    return ESP_OK;
}

void bt401_print_state(void)
{
    bt401_state_t st;
    bt401_parser_t p;
    int64_t last;
    portENTER_CRITICAL(&s_lock);
    st = s_st; p = s_parser; last = s_last_line_us;
    portEXIT_CRITICAL(&s_lock);
    printf("task: %s, last line: %s\n", s_task ? "running" : "stopped",
           last ? "seen" : "never");
    if (last) printf("  last line %lld ms ago\n", (long long)((now_us() - last) / 1000));
    printf("status: TS+%02d  connected=%d  music=%d  call=%d  dialing=%d\n", st.ts < 0 ? 0 : st.ts,
           st.connected, st.music, (int)st.call, st.dialing);
    printf("caller id: '%s'  own name: '%s'  own address: '%s'\n", st.caller_id, st.name, st.addr);
    printf("parser: %u bytes, %u lines, %u overlong;  module errors (ER+): %u\n", (unsigned)p.bytes,
           (unsigned)p.lines, (unsigned)p.overflows, (unsigned)st.errors);
}
