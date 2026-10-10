/* bk3266.c — BK3266SR backend for bt_companion. See bk3266.h.
 *
 * Everything about the module's behaviour here comes from its documentation,
 * not from a real unit yet (none had arrived when this was written); the
 * places that are guesses are marked "UNVERIFIED" and listed in
 * archive/hardware/bk3266.md so they get checked in order on the bench. */
#include "bk3266.h"

#include "board.h"
#include "board_pins.h"
#include "bk3266_proto.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "bk3266";

#define BK_UART           UART_NUM_1
#define UART_RX_BUF       256
#define POLL_MS           1500   /* call-status query period */
#define LINK_TIMEOUT_MS   6000   /* no frame this long -> link_up = false */
#define BOOT_SETTLE_MS    1500   /* let the module finish booting before the first query */

static portMUX_TYPE     s_lock = portMUX_INITIALIZER_UNLOCKED;
static bk_state_t       s_st;
static bk_parser_t      s_parser;
static int64_t          s_last_frame_us;
static TaskHandle_t     s_task;
static volatile bool    s_stop;
static bool             s_uart_ready;
static bk3266_notify_cb s_notify;

static int64_t now_us(void) { return esp_timer_get_time(); }

/* ---- UART ---------------------------------------------------------------- */

static esp_err_t uart_ready(void)
{
    if (s_uart_ready) return ESP_OK;
    uart_config_t cfg = {
        .baud_rate = BK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = ESP_OK;
    if (!uart_is_driver_installed(BK_UART)) {
        err = uart_driver_install(BK_UART, UART_RX_BUF, 0, 0, NULL, 0);
    }
    if (err == ESP_OK) err = uart_param_config(BK_UART, &cfg);   /* also fixes the baud rate */
    if (err == ESP_OK) {
        err = uart_set_pin(BK_UART, BOARD_UART_TX, BOARD_UART_RX,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_uart_ready = true;
    return ESP_OK;
}

/* WAKEUP: the module powers itself on at boot, and the spreadsheet and the
 * reference schematic disagree about what a pulse on it does once it's on
 * (power on vs. disconnect/reconnect) -- so normal operation never touches
 * it. Left as a plain input; R17 on the main board holds it low. */
static void wake_release(void)
{
#ifdef BOARD_BT_MFB_DEDICATED
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << BOARD_BT_MFB_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_hold_dis(BOARD_BT_MFB_PIN);
    gpio_config(&c);
#endif
}

static void send_bytes(const uint8_t *b, size_t n)
{
    uart_write_bytes(BK_UART, (const char *)b, n);
}

static esp_err_t send_code_raw(uint8_t code)
{
    uint8_t f[BK_FRAME_MAX_BYTES];
    size_t n = bk_build_command(code, f, sizeof(f));
    if (!n || !s_uart_ready) return ESP_ERR_INVALID_STATE;
    send_bytes(f, n);
    return ESP_OK;
}

/* ---- state -> bt_companion types ----------------------------------------- */

static bt_call_state_t map_call(bk_call_t c)
{
    switch (c) {
    case BK_CALL_INCOMING: return BT_CALL_INCOMING;
    case BK_CALL_OUTGOING: return BT_CALL_OUTGOING;
    case BK_CALL_ACTIVE:   return BT_CALL_ACTIVE;
    default:               return BT_CALL_IDLE;
    }
}

void bk3266_get_status(bt_companion_status_t *out)
{
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    int64_t age_ms = s_last_frame_us ? (now_us() - s_last_frame_us) / 1000 : INT64_MAX;
    out->link_up = s_task && age_ms < LINK_TIMEOUT_MS;
    out->bt_connected = out->link_up && s_st.connected;
    out->call_state = out->link_up ? map_call(s_st.call) : BT_CALL_IDLE;
    strlcpy(out->paired_name, out->bt_connected ? s_st.phone_name : "", sizeof(out->paired_name));
    strlcpy(out->caller_id, out->link_up ? s_st.caller_id : "", sizeof(out->caller_id));
    strlcpy(out->bd_addr, s_st.local_addr, sizeof(out->bd_addr));
    portEXIT_CRITICAL(&s_lock);
}

/* ---- task ---------------------------------------------------------------- */

static void handle_frame(const bk_frame_t *f, bt_call_state_t *last_call, char *last_caller)
{
    char desc[96];
    bk_describe(f, desc, sizeof(desc));

    portENTER_CRITICAL(&s_lock);
    int prev_status = s_st.last_status;
    unsigned chg = bk_state_apply(&s_st, f);
    s_last_frame_us = now_us();
    bt_call_state_t call = map_call(s_st.call);
    char caller[BK_NUMBER_MAX];
    strlcpy(caller, s_st.caller_id, sizeof(caller));
    portEXIT_CRITICAL(&s_lock);

    if ((chg & BK_CHG_LINK) && s_st.connected) board_led_set_state(BOARD_LED_BT_CONNECTED);

    /* Every periodic query reply is a frame; only log the ones that mean
     * something changed (or aren't plain status replies). */
    if (chg || f->cmd != BK_MID_ACK || (f->n && f->data[0] != prev_status)) {
        ESP_LOGI(TAG, "rx: %s", desc);
    } else {
        ESP_LOGD(TAG, "rx: %s", desc);
    }

    bool incoming_caller_changed = call == BT_CALL_INCOMING && strcmp(caller, last_caller) != 0;
    if (call != *last_call || incoming_caller_changed) {
        *last_call = call;
        strlcpy(last_caller, caller, BK_NUMBER_MAX);
        if (s_notify) s_notify(call);
    }
}

static void bk_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    bt_call_state_t last_call = BT_CALL_IDLE;
    char last_caller[BK_NUMBER_MAX] = "";
    int64_t t0 = now_us();
    int64_t next_poll = t0 + (int64_t)BOOT_SETTLE_MS * 1000;
    int stage = 0;             /* startup queries, one per poll */
    bool rec_mode_sent = false;

    ESP_LOGI(TAG, "listening at %d baud on GPIO%d/GPIO%d", BK_BAUD, (int)BOARD_UART_TX, (int)BOARD_UART_RX);

    while (!s_stop) {
        int n = uart_read_bytes(BK_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            bk_frame_t f;
            if (bk_parser_feed(&s_parser, buf[i], &f)) handle_frame(&f, &last_call, last_caller);
        }

        int64_t now = now_us();
        if (now < next_poll) continue;
        next_poll = now + (int64_t)POLL_MS * 1000;

        /* Startup information, one query per tick so replies don't pile up. */
        static const uint8_t startup[] = { BK_CMD_VISON, BK_CMD_DEVICE_ADDR, BK_CMD_DEVICE_NAME };
        if (stage < (int)sizeof(startup)) {
            send_code_raw(startup[stage++]);
            continue;
        }

        /* UNVERIFIED: the module may power up in transmit mode (the S/R pin
         * is the indicator), and RECMODE is how the documentation says to
         * select receive. Sent once, after the module has answered. */
        if (!rec_mode_sent && s_last_frame_us) {
            rec_mode_sent = true;
            ESP_LOGI(TAG, "selecting receive mode");
            send_code_raw(BK_CMD_RECMODE);
            continue;
        }

        /* Ask for the call state rather than wait for it to be volunteered:
         * the table has no "call ended" event, so this is how an ended or
         * missed call is noticed (M1 = linked, no call). */
        send_code_raw(BK_CMD_MY);

        portENTER_CRITICAL(&s_lock);
        bool want_name = s_st.connected && !s_st.phone_name[0];
        portEXIT_CRITICAL(&s_lock);
        if (want_name) send_code_raw(BK_CMD_PHONE_NAME);
    }

    /* Told the listener nothing while stopping: bk3266_stop() does that. */
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t bk3266_start(bk3266_notify_cb cb)
{
    if (s_task) return ESP_OK;
    esp_err_t err = uart_ready();
    if (err != ESP_OK) return err;
    wake_release();
    uart_flush_input(BK_UART);
    s_notify = cb;
    portENTER_CRITICAL(&s_lock);
    bk_state_init(&s_st);
    s_last_frame_us = 0;
    portEXIT_CRITICAL(&s_lock);
    bk_parser_init(&s_parser);
    s_stop = false;
    if (xTaskCreate(bk_task, "bk3266", 3072, NULL, tskIDLE_PRIORITY + 1, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void bk3266_stop(bool disconnect)
{
    if (!s_task) return;
    if (disconnect) {
        send_code_raw(BK_CMD_DISCONNECTED);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    s_stop = true;
    for (int waited = 0; s_task && waited < 1000; waited += 50) vTaskDelay(pdMS_TO_TICKS(50));
    bool had_call;
    portENTER_CRITICAL(&s_lock);
    had_call = s_st.call != BK_CALL_IDLE;
    bk_state_init(&s_st);
    s_last_frame_us = 0;
    portEXIT_CRITICAL(&s_lock);
    if (had_call && s_notify) s_notify(BT_CALL_IDLE);  /* don't leave a bell ringing */
}

bool bk3266_running(void) { return s_task != NULL; }

/* ---- call control -------------------------------------------------------- */

static esp_err_t command_if_linked(uint8_t code, bool need_phone)
{
    bt_companion_status_t st;
    bk3266_get_status(&st);
    if (!st.link_up || (need_phone && !st.bt_connected)) return ESP_ERR_INVALID_STATE;
    return send_code_raw(code);
}

esp_err_t bk3266_accept_call(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ringing = s_st.call == BK_CALL_INCOMING;
    portEXIT_CRITICAL(&s_lock);
    return ringing ? command_if_linked(BK_CMD_ACK, true) : ESP_ERR_NOT_FOUND;
}

esp_err_t bk3266_reject_call(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ringing = s_st.call == BK_CALL_INCOMING;
    portEXIT_CRITICAL(&s_lock);
    return ringing ? command_if_linked(BK_CMD_HFHUNG, true) : ESP_ERR_NOT_FOUND;
}

esp_err_t bk3266_end_call(void)
{
    portENTER_CRITICAL(&s_lock);
    bool live = s_st.call == BK_CALL_OUTGOING || s_st.call == BK_CALL_ACTIVE;
    portEXIT_CRITICAL(&s_lock);
    return live ? command_if_linked(BK_CMD_NACK, true) : ESP_ERR_NOT_FOUND;
}

esp_err_t bk3266_start_pairing(void)
{
    return command_if_linked(BK_CMD_MATCH, false);
}

bool bk3266_call_live(void)
{
    portENTER_CRITICAL(&s_lock);
    bool live = s_st.call == BK_CALL_OUTGOING || s_st.call == BK_CALL_ACTIVE;
    portEXIT_CRITICAL(&s_lock);
    return live;
}

/* ---- bring-up ------------------------------------------------------------ */

esp_err_t bk3266_send_code(uint8_t code)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    return send_code_raw(code);
}

void bk3266_print_state(void)
{
    bk_state_t st;
    bk_parser_t p;
    int64_t last;
    portENTER_CRITICAL(&s_lock);
    st = s_st;
    p = s_parser;
    last = s_last_frame_us;
    portEXIT_CRITICAL(&s_lock);
    static const char *const call_name[] = { "idle", "incoming", "outgoing", "active" };
    printf("task        : %s\n", s_task ? "running" : "not running");
    printf("last frame  : %s\n", last ? "" : "none yet");
    if (last) printf("              %lld ms ago\n", (long long)((now_us() - last) / 1000));
    printf("booted      : %s\n", st.booted ? "yes (saw the boot status)" : "no boot status seen");
    printf("connected   : %s\n", st.connected ? "yes" : "no");
    printf("call        : %s\n", call_name[st.call]);
    printf("caller id   : '%s'\n", st.caller_id);
    printf("phone name  : '%s'\n", st.phone_name);
    printf("own name    : '%s'\n", st.local_name);
    printf("own address : '%s'\n", st.local_addr);
    printf("version     : '%s'\n", st.version);
    printf("volume      : %d\n", st.volume);
    if (st.last_status >= 0) {
        printf("last status : 0x%02X %s\n", st.last_status, bk_status_name((uint8_t)st.last_status));
    }
    printf("parser      : %lu bytes, %lu good frames, %lu bad checksums, %lu bytes skipped\n",
           (unsigned long)p.bytes, (unsigned long)p.frames, (unsigned long)p.crc_errors,
           (unsigned long)p.resyncs);
}

/* Prints bytes as they arrive, and each frame the moment it's complete.
 * Returns the number of bytes seen. */
static int listen_print(int ms, int *printable_out)
{
    bk_parser_t p;
    bk_parser_init(&p);
    int total = 0, printable = 0, col = 0;
    int64_t end = now_us() + (int64_t)ms * 1000;
    while (now_us() < end) {
        uint8_t b;
        if (uart_read_bytes(BK_UART, &b, 1, pdMS_TO_TICKS(20)) != 1) continue;
        total++;
        if ((b >= 0x20 && b < 0x7F) || b == '\r' || b == '\n') printable++;
        printf("%02X ", b);
        if (++col == 16) { printf("\n"); col = 0; }
        bk_frame_t f;
        if (bk_parser_feed(&p, b, &f)) {
            char d[96];
            bk_describe(&f, d, sizeof(d));
            printf("\n  -> frame: %s\n", d);
            col = 0;
        }
    }
    if (col) printf("\n");
    if (printable_out) *printable_out = printable;
    return total;
}

static esp_err_t bring_up_ready(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    esp_err_t err = uart_ready();
    if (err != ESP_OK) return err;
    wake_release();
    uart_flush_input(BK_UART);
    return ESP_OK;
}

esp_err_t bk3266_listen(int listen_ms)
{
    esp_err_t err = bring_up_ready();
    if (err != ESP_OK) return err;
    printf("listening %d ms at %d baud (GPIO%d = module TX)...\n", listen_ms, BK_BAUD, (int)BOARD_UART_RX);
    int n = listen_print(listen_ms, NULL);
    printf("%d byte%s received\n", n, n == 1 ? "" : "s");
    return ESP_OK;
}

esp_err_t bk3266_send_raw(const uint8_t *tx, size_t n, int listen_ms)
{
    if (s_task) {
        /* The driver owns the UART and logs/decodes whatever comes back
         * (see bk_state), so just put the bytes on the wire. */
        send_bytes(tx, n);
        printf("sent %u byte%s; the driver is running: watch the log or bk_state for the reply\n",
               (unsigned)n, n == 1 ? "" : "s");
        return ESP_OK;
    }
    esp_err_t err = bring_up_ready();
    if (err != ESP_OK) return err;
    printf("sending %u byte%s, listening %d ms...\n", (unsigned)n, n == 1 ? "" : "s", listen_ms);
    send_bytes(tx, n);
    int got = listen_print(listen_ms, NULL);
    printf("%d byte%s received\n", got, got == 1 ? "" : "s");
    return ESP_OK;
}

esp_err_t bk3266_send_framed(uint8_t cmd, const uint8_t *data, size_t n, int listen_ms)
{
    uint8_t f[BK_FRAME_MAX_BYTES];
    size_t len = bk_build_frame(cmd, data, n, f, sizeof(f));
    if (!len) return ESP_ERR_INVALID_ARG;
    printf("frame:");
    for (size_t i = 0; i < len; i++) printf(" %02X", f[i]);
    printf("\n");
    return bk3266_send_raw(f, len, listen_ms);
}

esp_err_t bk3266_probe(void)
{
    esp_err_t err = bring_up_ready();
    if (err != ESP_OK) return err;

    printf("1/3 listening 2 s for anything the module sends on its own (boot status)...\n");
    int printable = 0;
    int boot = listen_print(2000, &printable);

    /* Two readings of the spreadsheet for how a command goes out:
     * CMD=0x01 with the command code as DATA (its worked examples), or the
     * command code itself as CMD. Same harmless query either way. */
    uint8_t f[BK_FRAME_MAX_BYTES];
    size_t n = bk_build_command(BK_CMD_VISON, f, sizeof(f));
    printf("2/3 version query, command code as DATA (cmd 0x01):\n");
    send_bytes(f, n);
    int a = listen_print(1000, NULL);

    n = bk_build_frame(BK_CMD_VISON, NULL, 0, f, sizeof(f));
    printf("3/3 version query, command code as CMD:\n");
    send_bytes(f, n);
    int b = listen_print(1000, NULL);

    printf("\nresult: %d byte(s) unprompted, %d after query 2, %d after query 3\n", boot, a, b);
    if (boot + a + b == 0) {
        printf("  nothing at all -> power (3.3 V on J2 pin 1?), TX/RX swapped, or a different baud rate\n"
               "  next: bk_listen 5000 while pressing a button on the module, check module pin 10 with a scope\n");
    } else if (printable > 0 && printable >= boot / 2 && boot > 0) {
        printf("  mostly printable text -> this looks like the text AT firmware, not the binary\n"
               "  protocol. Try: bk_at BT+VER   (see archive/hardware/bk3266.md)\n");
    } else {
        printf("  bytes arrived -> check above for decoded frames. 'bk_start' runs the real driver.\n");
    }
    return ESP_OK;
}

esp_err_t bk3266_wake_pulse(int ms)
{
#ifdef BOARD_BT_MFB_DEDICATED
    if (s_task) return ESP_ERR_INVALID_STATE;
    if (ms < 10) ms = 10;
    if (ms > 4000) ms = 4000;   /* a 6 s hold powers the module off: stay well under */
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << BOARD_BT_MFB_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_hold_dis(BOARD_BT_MFB_PIN);
    gpio_config(&c);
    gpio_sleep_sel_dis(BOARD_BT_MFB_PIN);
    gpio_set_level(BOARD_BT_MFB_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(BOARD_BT_MFB_PIN, 0);
    wake_release();
    return ESP_OK;
#else
    (void)ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
