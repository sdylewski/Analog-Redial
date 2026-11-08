/* coinboard.c -- see coinboard.h. */
#include "coinboard.h"
#include "coin_inputs.h"
#include "coin_fire.h"
#include "board.h"
#include "payphone.h"
#include "telephony.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "coinboard";

/* TCA9534A registers */
#define REG_INPUT    0x00
#define REG_OUTPUT   0x01
#define REG_POLARITY 0x02
#define REG_CONFIG   0x03

/* ADS1115 registers */
#define ADS_REG_CONV 0x00
#define ADS_REG_CFG  0x01

/* The board sits at the end of a cable: 100 kHz, like the rest of the bus. */
#define CB_I2C_HZ        100000
#define CB_I2C_TIMEOUT   20      /* ms */
#define CB_VERIFY_MS     1000
#define CB_RETRY_MS      1000
#define CB_FAILS_TO_OFF  30      /* about 100 ms of failed polls */
#define CB_IDLE_WAIT_MS  200     /* switched off: how often to look again */
/* U4: single conversions at 860 samples/s take about 1.2 ms. */
#define ADS_CONV_US      1500
#define ADS_IDLE_MS      100     /* idle: one channel this often, for the web UI */
#define ADS_RETRY_MS     5000
#define ADS_FAILS_TO_OFF 10

#define NVS_NS      "coinboard"
#define KEY_CFG     "cfg"
#define CFG_VERSION 1

typedef struct {
    uint8_t         version;
    coinboard_cfg_t cfg;
} stored_cfg_t;

static const coinboard_cfg_t DEFAULTS = {
    .enabled = false,
    .line = 0,
    .auto_relay = true,
    .swap = false,
    .pulse_ms = CF_PULSE_DEFAULT_MS,
    .coin4_cents = 0,
};

static SemaphoreHandle_t       s_lock;   /* s_cfg, s_fire, s_cin */
static coinboard_cfg_t         s_cfg;
static coin_fire_t             s_fire;
static coin_in_t               s_cin;
static TaskHandle_t            s_task;
static i2c_master_dev_handle_t s_io, s_adc;

static atomic_bool   s_io_ok, s_adc_ok, s_active;
static atomic_uchar  s_input = 0xFF;   /* everything open until a read says otherwise */
static atomic_uchar  s_sent_output;
static atomic_uint   s_bus_errors, s_chip_resets, s_coins;
static atomic_int    s_adc_raw[4];
static atomic_bool   s_adc_have[4];

/* Only the task touches these. */
static int      s_fails;
static int      s_mismatch;     /* polls in a row where an output pin didn't read back */
static int64_t  s_last_verify_ms, s_last_retry_ms;
static int      s_adc_pending = -1;
static int64_t  s_adc_started_us;
static int64_t  s_adc_last_idle_ms, s_adc_retry_ms;
static int      s_adc_fails, s_adc_rr, s_adc_k;
static uint32_t s_results_seen;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
static inline void wake_task(void) { if (s_task) xTaskNotifyGive(s_task); }

/* ---- conversions ---- */

/* +-4.096 V over 16 bits: 125 uV a count. Noise below zero reads 0. */
static int64_t ain_uv(int raw) { return raw > 0 ? (int64_t)raw * 125 : 0; }
int coinboard_ain_mv(int raw)    { return (int)(ain_uv(raw) / 1000); }
int coinboard_rail_mv(int raw)   { return (int)(ain_uv(raw) * 177 / 27 / 1000); }     /* (150k + 27k) / 27k */
int coinboard_hv_mv(int raw)     { return (int)(ain_uv(raw) * 1027 / 27 / 1000); }    /* (1M + 27k) / 27k */
int coinboard_isense_ma(int raw) { return (int)(ain_uv(raw) / 10 / 1000); }           /* 10 ohm */

const char *coinboard_input_name(int bit)
{
    switch (bit) {
        case COINBOARD_BIT_COIN4:   return "coin 4";
        case COINBOARD_BIT_QUARTER: return "quarter";
        case COINBOARD_BIT_DIME:    return "dime";
        case COINBOARD_BIT_NICKEL:  return "nickel";
        default:                    return "?";
    }
}

/* ---- bus ---- */

static esp_err_t io_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(s_io, buf, sizeof(buf), CB_I2C_TIMEOUT);
    if (err != ESP_OK) atomic_fetch_add(&s_bus_errors, 1);
    return err;
}

static esp_err_t io_read(uint8_t reg, uint8_t *val)
{
    esp_err_t err = i2c_master_transmit_receive(s_io, &reg, 1, val, 1, CB_I2C_TIMEOUT);
    if (err != ESP_OK) atomic_fetch_add(&s_bus_errors, 1);
    return err;
}

static uint8_t out_bits(const cf_out_t *o)
{
    return (uint8_t)((o->en_hv ? 1u << COINBOARD_BIT_EN_HV : 0) |
                     (o->pol   ? 1u << COINBOARD_BIT_POL   : 0) |
                     (o->fire  ? 1u << COINBOARD_BIT_FIRE  : 0));
}

/* Output register first, then direction: the chip powers up with 0xFF in its
 * output register, and the other order would drive EN_HV, POL and FIRE high
 * for a moment. */
static esp_err_t io_setup(uint8_t out)
{
    uint8_t readback = 0;
    esp_err_t err = io_write(REG_OUTPUT, out);
    if (err == ESP_OK) err = io_write(REG_POLARITY, 0x00);
    if (err == ESP_OK) err = io_write(REG_CONFIG, COINBOARD_CONFIG);
    if (err == ESP_OK) err = io_read(REG_CONFIG, &readback);
    if (err == ESP_OK && readback != COINBOARD_CONFIG) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        atomic_store(&s_sent_output, out);
        s_fails = 0;
    }
    return err;
}

static esp_err_t add_devices(void)
{
    if (s_io) return ESP_OK;
    esp_err_t err = board_i2c_init();
    if (err != ESP_OK) return err;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = COINBOARD_IO_ADDR,
        .scl_speed_hz = CB_I2C_HZ,
    };
    err = i2c_master_bus_add_device(board_i2c_bus(), &dc, &s_io);
    if (err != ESP_OK) { s_io = NULL; return err; }
    dc.device_address = COINBOARD_ADC_ADDR;
    err = i2c_master_bus_add_device(board_i2c_bus(), &dc, &s_adc);
    if (err != ESP_OK) s_adc = NULL;
    return ESP_OK;
}

/* ---- U4 ---- */

static uint16_t ads_config(int ch)
{
    /* OS = start, MUX = AINch to GND, PGA +-4.096 V, single shot, 860 SPS,
     * comparator off. */
    return (uint16_t)(0x8000 | ((4 + ch) << 12) | (1 << 9) | (1 << 8) | (7 << 5) | 3);
}

static void adc_lost(const char *why)
{
    atomic_store(&s_adc_ok, false);
    for (int i = 0; i < 4; i++) atomic_store(&s_adc_have[i], false);
    s_adc_pending = -1;
    ESP_LOGW(TAG, "ADS1115 at 0x%02X %s: HV and the coil current can't be measured",
             COINBOARD_ADC_ADDR, why);
}

/* One step: collect the conversion that's ready, start the next. Which
 * channel depends on what the relay is doing: the coil current while firing,
 * HV and the rail while charging, all four slowly when idle. */
static void adc_service(int64_t now_us, cf_state_t st)
{
    int64_t now_ms = now_us / 1000;
    if (!s_adc) return;
    if (!atomic_load(&s_adc_ok)) {
        if (now_ms - s_adc_retry_ms < ADS_RETRY_MS && s_adc_retry_ms) return;
        s_adc_retry_ms = now_ms;
        if (i2c_master_probe(board_i2c_bus(), COINBOARD_ADC_ADDR, CB_I2C_TIMEOUT) != ESP_OK) return;
        atomic_store(&s_adc_ok, true);
        s_adc_fails = 0;
        s_adc_pending = -1;
        ESP_LOGI(TAG, "ADS1115 at 0x%02X answering", COINBOARD_ADC_ADDR);
    }

    if (s_adc_pending >= 0) {
        if (now_us - s_adc_started_us < ADS_CONV_US) return;
        uint8_t reg = ADS_REG_CONV, b[2];
        if (i2c_master_transmit_receive(s_adc, &reg, 1, b, 2, CB_I2C_TIMEOUT) == ESP_OK) {
            atomic_store(&s_adc_raw[s_adc_pending], (int16_t)((b[0] << 8) | b[1]));
            atomic_store(&s_adc_have[s_adc_pending], true);
            s_adc_fails = 0;
        } else {
            atomic_fetch_add(&s_bus_errors, 1);
            if (++s_adc_fails >= ADS_FAILS_TO_OFF) { adc_lost("stopped answering"); return; }
        }
        s_adc_pending = -1;
    }

    int ch;
    switch (st) {
        case CF_FIRE:
        case CF_DRAIN:  ch = (s_adc_k++ % 4 == 3) ? COINBOARD_AIN_HV : COINBOARD_AIN_ISENSE; break;
        case CF_CHARGE: ch = (s_adc_k++ % 2) ? COINBOARD_AIN_RAIL : COINBOARD_AIN_HV; break;
        default:
            if (now_ms - s_adc_last_idle_ms < ADS_IDLE_MS) return;
            s_adc_last_idle_ms = now_ms;
            ch = s_adc_rr++ % 4;
            break;
    }
    uint16_t c = ads_config(ch);
    uint8_t w[3] = { ADS_REG_CFG, (uint8_t)(c >> 8), (uint8_t)c };
    if (i2c_master_transmit(s_adc, w, sizeof(w), CB_I2C_TIMEOUT) == ESP_OK) {
        s_adc_pending = ch;
        s_adc_started_us = now_us;
    } else {
        atomic_fetch_add(&s_bus_errors, 1);
        if (++s_adc_fails >= ADS_FAILS_TO_OFF) adc_lost("stopped answering");
    }
}

static cf_meas_t measurements(void)
{
    cf_meas_t m = { 0 };
    m.adc = atomic_load(&s_adc_ok) && atomic_load(&s_adc_have[COINBOARD_AIN_RAIL]) &&
            atomic_load(&s_adc_have[COINBOARD_AIN_HV]) && atomic_load(&s_adc_have[COINBOARD_AIN_ISENSE]);
    m.rail_mv = coinboard_rail_mv(atomic_load(&s_adc_raw[COINBOARD_AIN_RAIL]));
    m.hv_mv = coinboard_hv_mv(atomic_load(&s_adc_raw[COINBOARD_AIN_HV]));
    m.i_ma = coinboard_isense_ma(atomic_load(&s_adc_raw[COINBOARD_AIN_ISENSE]));
    return m;
}

/* ---- the task ---- */

static void go_offline(const char *why, int64_t now_ms)
{
    lock();
    coin_fire_abort(&s_fire, "the coin board stopped answering", now_ms);
    unlock();
    atomic_store(&s_io_ok, false);
    atomic_store(&s_input, 0xFF);
    ESP_LOGE(TAG, "coin board at 0x%02X %s", COINBOARD_IO_ADDR, why);
    payphone_logf(s_cfg.line, "coin board %s", why);
    /* A device stuck mid-byte holding SDA low would take the codec's bus down too. */
    i2c_master_bus_reset(board_i2c_bus());
}

static void on_coins(uint8_t coins, uint8_t stuck, int line, uint8_t coin4)
{
    for (int b = 0; b < COIN_IN_N; b++) {
        if (stuck & (1u << b))
            payphone_logf(line, "coin board: %s input stuck closed (a jammed coin or a shorted wire?)",
                          coinboard_input_name(b));
        if (!(coins & (1u << b))) continue;
        atomic_fetch_add(&s_coins, 1);
        int cents = b == COINBOARD_BIT_QUARTER ? 25 : b == COINBOARD_BIT_DIME ? 10 :
                    b == COINBOARD_BIT_NICKEL ? 5 : coin4;
        if (cents == 0) {
            payphone_logf(line, "coin board: coin 4 input (no value set)");
            continue;
        }
        esp_err_t err = telephony_coin_inject(line, cents, PAYPHONE_SRC_BOARD);
        if (err != ESP_OK) ESP_LOGW(TAG, "%d c coin not passed on: %s", cents, esp_err_to_name(err));
    }
}

static void service_once(int64_t now_us)
{
    int64_t now_ms = now_us / 1000;
    lock();
    coinboard_cfg_t cfg = s_cfg;
    bool busy = coin_fire_busy(&s_fire);
    unlock();
    bool want = cfg.enabled && payphone_enabled();
    atomic_store(&s_active, want);

    if (!want) {
        if (!atomic_load(&s_io_ok)) return;
        if (busy) {
            /* Switched off mid-sequence: finish it safely, then let go. */
            lock();
            cf_state_t fs = s_fire.state;
            if (fs == CF_POL || fs == CF_CHARGE || fs == CF_FIRE)
                coin_fire_abort(&s_fire, "the coin board was switched off", now_ms);
            unlock();
        } else {
            io_write(REG_OUTPUT, 0x00);
            atomic_store(&s_io_ok, false);
            ESP_LOGI(TAG, "coin board switched off");
            return;
        }
    }

    if (!atomic_load(&s_io_ok)) {
        if (s_last_retry_ms && now_ms - s_last_retry_ms < CB_RETRY_MS) return;
        s_last_retry_ms = now_ms;
        if (add_devices() != ESP_OK) return;
        if (i2c_master_probe(board_i2c_bus(), COINBOARD_IO_ADDR, CB_I2C_TIMEOUT) != ESP_OK) return;
        lock();
        uint8_t out = out_bits(&s_fire.out);
        unlock();
        if (io_setup(out) != ESP_OK) return;
        lock();
        coin_in_init(&s_cin);
        unlock();
        atomic_store(&s_io_ok, true);
        s_last_verify_ms = now_ms;
        ESP_LOGI(TAG, "coin board at 0x%02X answering", COINBOARD_IO_ADDR);
        payphone_logf(cfg.line, "coin board answering");
        return;
    }

    bool failed = false;

    /* The relay first: it's the time-critical part. */
    cf_cfg_t fc = { .pulse_ms = cfg.pulse_ms, .swap = cfg.swap };
    cf_meas_t m = measurements();
    lock();
    coin_fire_step(&s_fire, &fc, &m, now_ms);
    uint8_t out = out_bits(&s_fire.out);
    cf_state_t st = s_fire.state;
    uint32_t results = s_fire.results;
    char msg[sizeof(s_fire.msg)];
    memcpy(msg, s_fire.msg, sizeof(msg));
    unlock();
    if (out != atomic_load(&s_sent_output)) {
        if (io_write(REG_OUTPUT, out) == ESP_OK) atomic_store(&s_sent_output, out);
        else failed = true;
    }
    if (results != s_results_seen) {
        s_results_seen = results;
        payphone_logf(cfg.line, "coin relay %s", msg);
    }

    /* Coin inputs. Only while the relay isn't firing would be kinder to the
     * bus, but a coin can drop at any time. */
    uint8_t in;
    bool check_now = false;
    if (io_read(REG_INPUT, &in) == ESP_OK) {
        atomic_store(&s_input, in);
        /* The input register reads every pin, outputs included: an output
         * that doesn't read what was written means the chip reset (all
         * inputs, the pulldowns hold them low) or a pin is held by a fault.
         * Two polls, so a write landing between them isn't mistaken. */
        uint8_t sent = atomic_load(&s_sent_output);
        if (((in ^ sent) & (uint8_t)~COINBOARD_INPUTS) && out == sent) {
            if (++s_mismatch >= 2) check_now = true;
        } else {
            s_mismatch = 0;
        }
        uint8_t closed = (uint8_t)(~in & COINBOARD_INPUTS), stuck = 0;
        lock();
        uint8_t coins = coin_in_push(&s_cin, closed, now_ms, &stuck);
        unlock();
        if ((coins || stuck) && want) on_coins(coins, stuck, cfg.line, cfg.coin4_cents);
    } else {
        failed = true;
    }

    adc_service(now_us, st);

    /* No reset pin, and its own supply: it can come back as all inputs
     * without the ESP32 knowing. All inputs means the pulldowns hold the
     * outputs low, so that's safe, but a sequence in progress has to stop. */
    if (!failed && (check_now || now_ms - s_last_verify_ms >= CB_VERIFY_MS)) {
        s_last_verify_ms = now_ms;
        uint8_t got;
        if (io_read(REG_CONFIG, &got) != ESP_OK) {
            failed = true;
        } else if (got == COINBOARD_CONFIG && check_now) {
            /* Set up as it should be, yet a pin won't follow: something on the
             * board holds it. Stop, and say which. */
            uint8_t bad = (uint8_t)((in ^ atomic_load(&s_sent_output)) & (uint8_t)~COINBOARD_INPUTS);
            s_mismatch = 0;
            lock();
            cf_state_t fs = s_fire.state;
            if (fs == CF_POL || fs == CF_CHARGE || fs == CF_FIRE)
                coin_fire_abort(&s_fire, bad & (1u << COINBOARD_BIT_FIRE) ? "FIRE doesn't follow (P4 held?)" :
                                         bad & (1u << COINBOARD_BIT_EN_HV) ? "EN_HV doesn't follow (P6 held?)" :
                                         bad & (1u << COINBOARD_BIT_POL) ? "POL doesn't follow (P5 held?)" :
                                         "P7 doesn't follow", now_ms);
            unlock();
            ESP_LOGW(TAG, "coin board outputs read 0x%02X, wrote 0x%02X", in & 0xF0,
                     atomic_load(&s_sent_output));
        } else if (got != COINBOARD_CONFIG) {
            s_mismatch = 0;
            ESP_LOGW(TAG, "coin board lost its settings (config 0x%02X): restoring", got);
            atomic_fetch_add(&s_chip_resets, 1);
            lock();
            coin_fire_abort(&s_fire, "the coin board lost power", now_ms);
            coin_fire_step(&s_fire, &fc, &m, now_ms);
            uint8_t safe = out_bits(&s_fire.out);
            coin_in_init(&s_cin);
            unlock();
            if (io_setup(safe) != ESP_OK) failed = true;
        }
    }

    if (!failed) s_fails = 0;
    else if (++s_fails >= CB_FAILS_TO_OFF) go_offline("stopped answering", now_ms);
}

static void coinboard_task(void *arg)
{
    (void)arg;
    while (1) {
        bool on = atomic_load(&s_active) || atomic_load(&s_io_ok);
        /* A request's notify wakes this early, so a test starts at once. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(on ? COINBOARD_POLL_MS : CB_IDLE_WAIT_MS));
        service_once(esp_timer_get_time());
    }
}

/* ---- collect and return (payphone.h) ---- */

static void relay_cb(int line, payphone_relay_t what)
{
    coinboard_cfg_t c = coinboard_cfg();
    if (!c.enabled || line != c.line) return;
    if (!c.auto_relay) {
        payphone_logf(line, "coin board: automatic collect and return are off");
        return;
    }
    if (!atomic_load(&s_io_ok)) {
        payphone_logf(line, "coin board not answering: coins not %s",
                      what == PAYPHONE_COLLECT ? "collected" : "returned");
        return;
    }
    lock();
    coin_fire_request(&s_fire, what == PAYPHONE_COLLECT ? CF_COLLECT : CF_RETURN);
    unlock();
    wake_task();
}

esp_err_t coinboard_test(cf_what_t what)
{
    if (!atomic_load(&s_active) || !atomic_load(&s_io_ok)) return ESP_ERR_INVALID_STATE;
    lock();
    bool ok = coin_fire_request(&s_fire, what);
    unlock();
    if (!ok) return ESP_ERR_INVALID_ARG;
    payphone_logf(s_cfg.line, "coin board: %s test", coin_fire_what_name(what));
    wake_task();
    return ESP_OK;
}

/* esp_restart() (OTA, a settings restart): leave nothing high. A panic
 * doesn't come through here; coinboard_init() covers that on the way up. */
static void shutdown_handler(void)
{
    if (s_io && atomic_load(&s_io_ok)) io_write(REG_OUTPUT, 0x00);
}

/* ---- settings and status ---- */

static void clamp(coinboard_cfg_t *c)
{
    if (c->line > 1) c->line = 0;
    if (c->pulse_ms < CF_PULSE_MIN_MS) c->pulse_ms = CF_PULSE_MIN_MS;
    if (c->pulse_ms > CF_PULSE_MAX_MS) c->pulse_ms = CF_PULSE_MAX_MS;
    if (c->coin4_cents > 100) c->coin4_cents = 0;
}

coinboard_cfg_t coinboard_cfg(void)
{
    lock();
    coinboard_cfg_t c = s_cfg;
    unlock();
    return c;
}

esp_err_t coinboard_cfg_set(const coinboard_cfg_t *cfg)
{
    stored_cfg_t st = { .version = CFG_VERSION, .cfg = *cfg };
    clamp(&st.cfg);
    lock();
    s_cfg = st.cfg;
    unlock();
    payphone_set_board_line(st.cfg.enabled ? st.cfg.line : -1);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_CFG, &st, sizeof(st));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    wake_task();
    return err;
}

bool coinboard_active(void) { return atomic_load(&s_active); }

void coinboard_status(coinboard_status_t *o)
{
    memset(o, 0, sizeof(*o));
    lock();
    o->enabled = s_cfg.enabled;
    o->state = s_fire.state;
    o->doing = s_fire.what;
    o->result = s_fire.result;
    memcpy(o->result_msg, s_fire.msg, sizeof(o->result_msg));
    o->result_msg[sizeof(o->result_msg) - 1] = '\0';
    o->fires = s_fire.fires;
    o->stuck = coin_in_stuck(&s_cin);
    unlock();
    o->active = atomic_load(&s_active);
    o->io_ok = atomic_load(&s_io_ok);
    o->adc_ok = atomic_load(&s_adc_ok);
    o->input = atomic_load(&s_input);
    o->output = atomic_load(&s_sent_output);
    o->closed = o->io_ok ? (uint8_t)(~o->input & COINBOARD_INPUTS) : 0;
    bool a = o->adc_ok;
    o->rail_mv = a && atomic_load(&s_adc_have[COINBOARD_AIN_RAIL]) ? coinboard_rail_mv(atomic_load(&s_adc_raw[COINBOARD_AIN_RAIL])) : -1;
    o->hv_mv   = a && atomic_load(&s_adc_have[COINBOARD_AIN_HV]) ? coinboard_hv_mv(atomic_load(&s_adc_raw[COINBOARD_AIN_HV])) : -1;
    o->i_ma    = a && atomic_load(&s_adc_have[COINBOARD_AIN_ISENSE]) ? coinboard_isense_ma(atomic_load(&s_adc_raw[COINBOARD_AIN_ISENSE])) : -1;
    o->ain3_mv = a && atomic_load(&s_adc_have[COINBOARD_AIN_SPARE]) ? coinboard_ain_mv(atomic_load(&s_adc_raw[COINBOARD_AIN_SPARE])) : -1;
    o->coins = atomic_load(&s_coins);
    o->bus_errors = atomic_load(&s_bus_errors);
    o->chip_resets = atomic_load(&s_chip_resets);
}

void coinboard_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    coin_fire_init(&s_fire);
    coin_in_init(&s_cin);
    s_cfg = DEFAULTS;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        stored_cfg_t st;
        size_t len = sizeof(st);
        if (nvs_get_blob(h, KEY_CFG, &st, &len) == ESP_OK && len == sizeof(st) &&
            st.version == CFG_VERSION) {
            s_cfg = st.cfg;
            clamp(&s_cfg);
        }
        nvs_close(h);
    }
    payphone_set_board_line(s_cfg.enabled ? s_cfg.line : -1);
    payphone_set_relay_handler(relay_cb);
    esp_register_shutdown_handler(shutdown_handler);

    if (s_cfg.enabled && add_devices() == ESP_OK &&
        i2c_master_probe(board_i2c_bus(), COINBOARD_IO_ADDR, CB_I2C_TIMEOUT) == ESP_OK) {
        /* A crash mid-pulse leaves the chip's outputs as they were. */
        if (io_setup(0x00) == ESP_OK) ESP_LOGI(TAG, "coin board at 0x%02X: outputs off", COINBOARD_IO_ADDR);
    }
    ESP_LOGI(TAG, "coin board %s", s_cfg.enabled ? "enabled" : "not enabled");

    BaseType_t ok = xTaskCreate(coinboard_task, "coinboard", 3072, NULL,
                                configMAX_PRIORITIES - 6, &s_task);
    if (ok != pdPASS) ESP_LOGE(TAG, "task create failed");
}
