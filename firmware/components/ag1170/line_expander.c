/* line_expander.c -- see line_expander.h. */
#include "line_expander.h"
#include "board_config.h"

#if BOARD_LINE1_ON_EXPANDER

#include "board.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "line_exp";

/* TCA9534A registers */
#define REG_INPUT    0x00
#define REG_OUTPUT   0x01
#define REG_POLARITY 0x02
#define REG_CONFIG   0x03

/* Same 100 kHz as the ES8388: this bus can have the coin board's cable on it
 * (J11), which rules out 400 kHz. */
#define EXP_I2C_HZ      100000
#define EXP_I2C_TIMEOUT 20  /* ms */

/* How often SHK is read. A read is about 0.4 ms on the bus. The shortest
 * thing that has to be seen is a dial pulse's 15 ms minimum break
 * (pulse_break_min_ms); the contact bounce it filters out is 3-5 ms. */
#define EXP_POLL_MS         4
/* The chip has no reset pin and can lose power without the ESP32 resetting,
 * which puts it back to all inputs. Its config register shows that. */
#define EXP_VERIFY_MS       1000
#define EXP_RETRY_MS        1000
/* About 200 ms of failed reads: the line then reads on-hook. */
#define EXP_FAILS_TO_OFFLINE 50

/* The chip has no internal pull-ups, so a pin left as an input floats. SHK
 * has R12 and R13. PD is an input only while the SLIC runs, through D2 like
 * line 0's GPIO. Everything else is an output, including J10's spare P4-P7,
 * which stay low until something uses them. */
#define CONFIG_RUN        (uint8_t)((1u << BOARD_EXP_BIT_SHK) | (1u << BOARD_EXP_BIT_PD))
#define CONFIG_POWERDOWN  (uint8_t)(1u << BOARD_EXP_BIT_SHK)

static i2c_master_dev_handle_t s_dev;
static TaskHandle_t  s_task;
static atomic_uchar  s_want_output; /* every bit 0: F/R and RM low, PD's driver low */
static atomic_uchar  s_want_config = CONFIG_RUN;
static atomic_uchar  s_input;       /* last read; SHK low (on-hook) until a read says otherwise */
static atomic_bool   s_chip_ok;
static atomic_uint   s_bus_errors;
static atomic_uint   s_chip_resets;

/* Only the task touches these, plus line_exp_init() before it starts. */
static uint8_t s_sent_output;
static uint8_t s_sent_config;
static int     s_read_fails;
static int64_t s_last_read_us;
static int64_t s_last_verify_us;
static int64_t s_last_retry_us;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), EXP_I2C_TIMEOUT);
    if (err != ESP_OK) atomic_fetch_add(&s_bus_errors, 1);
    return err;
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, EXP_I2C_TIMEOUT);
    if (err != ESP_OK) atomic_fetch_add(&s_bus_errors, 1);
    return err;
}

/* The chip keeps its registers through an ESP32 reset, so this can find a line
 * left ringing by a crash. Output register first, then direction: the other
 * order drives the power-on 0xFF onto the pins for a moment, which would be RM
 * and F/R high. */
static esp_err_t chip_setup(void)
{
    uint8_t out = atomic_load(&s_want_output);
    uint8_t cfg = atomic_load(&s_want_config);
    uint8_t readback = 0;
    esp_err_t err = write_reg(REG_OUTPUT, out);
    if (err == ESP_OK) err = write_reg(REG_POLARITY, 0x00);
    if (err == ESP_OK) err = write_reg(REG_CONFIG, cfg);
    if (err == ESP_OK) err = read_reg(REG_CONFIG, &readback);
    if (err == ESP_OK && readback != cfg) err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) return err;
    s_sent_output = out;
    s_sent_config = cfg;
    s_read_fails = 0;
    return ESP_OK;
}

static void go_offline(void)
{
    ESP_LOGE(TAG, "expander at 0x%02X stopped answering: line 1 now reads on-hook", BOARD_EXP_I2C_ADDR);
    atomic_store(&s_chip_ok, false);
    atomic_store(&s_input, 0);
    /* A device stuck mid-byte holding SDA low would take the codec's bus down too. */
    i2c_master_bus_reset(board_i2c_bus());
}

static void service_once(int64_t now)
{
    if (!atomic_load(&s_chip_ok)) {
        if (now - s_last_retry_us < EXP_RETRY_MS * 1000) return;
        s_last_retry_us = now;
        if (chip_setup() == ESP_OK) {
            ESP_LOGI(TAG, "expander at 0x%02X answering", BOARD_EXP_I2C_ADDR);
            atomic_store(&s_chip_ok, true);
            s_last_verify_us = now;
        }
        return;
    }

    /* Outputs first: they're what's time-critical while ringing. */
    uint8_t out = atomic_load(&s_want_output);
    if (out != s_sent_output && write_reg(REG_OUTPUT, out) == ESP_OK) s_sent_output = out;
    uint8_t cfg = atomic_load(&s_want_config);
    if (cfg != s_sent_config && write_reg(REG_CONFIG, cfg) == ESP_OK) s_sent_config = cfg;

    /* SHK means nothing while the SLIC is powered down, so leave the bus quiet. */
    bool running = (s_sent_config == CONFIG_RUN);
    if (running && now - s_last_read_us >= EXP_POLL_MS * 1000) {
        s_last_read_us = now;
        uint8_t in;
        if (read_reg(REG_INPUT, &in) == ESP_OK) {
            atomic_store(&s_input, in);
            s_read_fails = 0;
        } else if (++s_read_fails >= EXP_FAILS_TO_OFFLINE) {
            go_offline();
            return;
        }
    }

    if (now - s_last_verify_us >= EXP_VERIFY_MS * 1000) {
        s_last_verify_us = now;
        uint8_t got;
        if (read_reg(REG_CONFIG, &got) == ESP_OK && got != s_sent_config) {
            ESP_LOGW(TAG, "expander lost its settings (config 0x%02X, wrote 0x%02X): restoring",
                     got, s_sent_config);
            atomic_fetch_add(&s_chip_resets, 1);
            if (chip_setup() != ESP_OK) go_offline();
        }
    }
}

static void expander_task(void *arg)
{
    while (1) {
        /* A setter's notify wakes this early, so a new output goes out at once. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EXP_POLL_MS));
        service_once(esp_timer_get_time());
    }
}

static inline void wake_task(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

static void set_output_bit(int bit, int level)
{
    uint8_t mask = (uint8_t)(1u << bit);
    uint8_t before = level ? atomic_fetch_or(&s_want_output, mask)
                           : atomic_fetch_and(&s_want_output, (uint8_t)~mask);
    if (((before & mask) != 0) != (level != 0)) wake_task();
}

void line_exp_set_fr(int level) { set_output_bit(BOARD_EXP_BIT_FR, level); }
void line_exp_set_rm(int level) { set_output_bit(BOARD_EXP_BIT_RM, level); }

void line_exp_set_spare(int n, int level)
{
    if (n < 0 || n >= LINE_EXP_SPARE_N) return;
    set_output_bit(BOARD_EXP_BIT_SPARE0 + n, level);
}

int line_exp_spare(int n)
{
    if (n < 0 || n >= LINE_EXP_SPARE_N) return 0;
    return (atomic_load(&s_want_output) >> (BOARD_EXP_BIT_SPARE0 + n)) & 1;
}

bool line_exp_started(void) { return s_task != NULL; }

void line_exp_set_pd_run(bool run)
{
    atomic_store(&s_want_config, run ? CONFIG_RUN : CONFIG_POWERDOWN);
    wake_task();
}

int line_exp_shk(void)
{
    return (atomic_load(&s_input) >> BOARD_EXP_BIT_SHK) & 1;
}

void line_exp_status(line_exp_status_t *out)
{
    out->chip_ok = atomic_load(&s_chip_ok);
    out->input = atomic_load(&s_input);
    out->output = atomic_load(&s_want_output);
    out->config = atomic_load(&s_want_config);
    out->bus_errors = atomic_load(&s_bus_errors);
    out->chip_resets = atomic_load(&s_chip_resets);
}

esp_err_t line_exp_init(void)
{
    /* ag1170 starts it for a second line, auxpins for J10's pins: once. */
    if (s_task) return ESP_OK;
    esp_err_t err = board_i2c_init();
    if (err != ESP_OK) return err;
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_EXP_I2C_ADDR,
        .scl_speed_hz = EXP_I2C_HZ,
    };
    if (!s_dev) {
        err = i2c_master_bus_add_device(board_i2c_bus(), &dcfg, &s_dev);
        if (err != ESP_OK) return err;
    }

    if (chip_setup() == ESP_OK) {
        atomic_store(&s_chip_ok, true);
        ESP_LOGI(TAG, "expander at 0x%02X set up", BOARD_EXP_I2C_ADDR);
    } else {
        ESP_LOGE(TAG, "no expander answering at 0x%02X: line 1 reads on-hook and J10 stays "
                 "off until it does", BOARD_EXP_I2C_ADDR);
    }

    /* Below ag1170's monitor task, which never waits on this one. */
    BaseType_t ok = xTaskCreate(expander_task, "line_exp", 3072, NULL,
                                configMAX_PRIORITIES - 5, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

#endif /* BOARD_LINE1_ON_EXPANDER */
