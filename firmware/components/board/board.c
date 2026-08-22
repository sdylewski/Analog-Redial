#include "board.h"
#include "board_pins.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "board";
static i2c_master_bus_handle_t s_bus;
static board_codec_t s_detected_codec = BOARD_CODEC_NONE;

#define BOARD_NAME_NVS_NS  "board"
#define BOARD_NAME_NVS_KEY "name"
#define BOARD_NAME_MAX     32
#define BOARD_LANG_NVS_KEY "lang"
#define BOARD_LANG_MAX     8
#define BOARD_ROH_EN_KEY   "roh_en"
#define BOARD_ROH_SECS_KEY "roh_secs"

esp_err_t board_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = BOARD_I2C_SCL,
        .sda_io_num = BOARD_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }
    ESP_LOGI(TAG, "I2C bus up (SDA=%d SCL=%d, board=%s)", BOARD_I2C_SDA,
             BOARD_I2C_SCL, board_type_name(board_current_type()));
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

#define BOARD_ES8388_ADDR 0x10
#define BOARD_WM8731_PROBE_ADDR 0x1A

void board_detect_codec(void)
{
    if (board_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "board_detect_codec: I2C bus not available");
        return;
    }
    bool es8388_present = i2c_master_probe(s_bus, BOARD_ES8388_ADDR, 50) == ESP_OK;
    bool wm8731_present = i2c_master_probe(s_bus, BOARD_WM8731_PROBE_ADDR, 50) == ESP_OK;
    board_type_t built_for = board_current_type();

    if (es8388_present && !wm8731_present) {
        s_detected_codec = BOARD_CODEC_ES8388;
        ESP_LOGI(TAG, "codec detect: ES8388 @ 0x10 (SMD board)");
        if (built_for != BOARD_TYPE_SMD) {
            ESP_LOGW(TAG, "firmware built for '%s' but an SMD/ES8388 codec is on "
                          "the bus -- rebuild with CONFIG_BOARD_TYPE_SMD2=y",
                     board_type_name(built_for));
        }
    } else if (wm8731_present && !es8388_present) {
        s_detected_codec = BOARD_CODEC_WM8731;
        ESP_LOGI(TAG, "codec detect: WM8731 @ 0x1A (THT board)");
        if (built_for != BOARD_TYPE_THT) {
            ESP_LOGW(TAG, "firmware built for '%s' but a THT/WM8731 codec is on "
                          "the bus -- rebuild with CONFIG_BOARD_TYPE_THT=y",
                     board_type_name(built_for));
        }
    } else if (es8388_present && wm8731_present) {
        s_detected_codec = BOARD_CODEC_CONFLICT;
        ESP_LOGW(TAG, "codec detect: both 0x10 and 0x11/0x1A ACKed -- unexpected, "
                      "check for I2C bus contention or an address conflict");
    } else {
        s_detected_codec = BOARD_CODEC_NONE;
        ESP_LOGW(TAG, "codec detect: no codec found on I2C bus (built for '%s')",
                 board_type_name(built_for));
    }
}

board_codec_t board_detected_codec(void) { return s_detected_codec; }

const char *board_codec_name(board_codec_t c)
{
    switch (c) {
        case BOARD_CODEC_ES8388:   return "ES8388";
        case BOARD_CODEC_WM8731:   return "WM8731";
        case BOARD_CODEC_CONFLICT: return "conflict (two codecs?)";
        default:                    return "none detected";
    }
}

void board_serial_str(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    /* ESP_MAC_WIFI_STA is the base factory MAC -- present even before WiFi
     * starts, and stable across reflashes/NVS erases. */
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void board_hardware_str(char *out, size_t len)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    const char *chip =
        (info.model == CHIP_ESP32C6) ? "ESP32-C6" :
        (info.model == CHIP_ESP32C3) ? "ESP32-C3" : "ESP32";
    /* Prefer the physically detected codec; fall back to the built-for PCB's
     * codec if the probe hasn't run or found nothing (e.g. bench with no
     * codec attached). */
    const char *codec =
        (s_detected_codec == BOARD_CODEC_ES8388) ? "ES8388" :
        (s_detected_codec == BOARD_CODEC_WM8731) ? "WM8731" :
        (board_current_type() == BOARD_TYPE_SMD) ? "ES8388?" : "WM8731?";
    snprintf(out, len, "%s rev%d.%d \xc2\xb7 %s (%s)",
             chip, info.revision / 100, info.revision % 100, codec,
             board_current_type() == BOARD_TYPE_SMD ? "SMD2" : "through-hole");
}

void board_name_get(char *out, size_t len)
{
    if (len == 0) return;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(BOARD_NAME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        esp_err_t err = nvs_get_str(h, BOARD_NAME_NVS_KEY, out, &n);
        nvs_close(h);
        if (err == ESP_OK && out[0]) return;
    }
    /* Unset / first boot: fall back to the compile-time default hostname. */
    strncpy(out, CONFIG_MDNS_HOSTNAME, len - 1);
    out[len - 1] = '\0';
}

bool board_name_is_set(void)
{
    nvs_handle_t h;
    if (nvs_open(BOARD_NAME_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 0;
    esp_err_t err = nvs_get_str(h, BOARD_NAME_NVS_KEY, NULL, &n); /* size probe */
    nvs_close(h);
    return err == ESP_OK && n > 1; /* n counts the NUL, so >1 means non-empty */
}

esp_err_t board_name_set(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    char trimmed[BOARD_NAME_MAX];
    strncpy(trimmed, name, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(BOARD_NAME_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, BOARD_NAME_NVS_KEY, trimmed);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "board name set to '%s'", trimmed);
    return err;
}

void board_language_get(char *out, size_t len)
{
    if (len == 0) return;
    nvs_handle_t h;
    if (nvs_open(BOARD_NAME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        esp_err_t err = nvs_get_str(h, BOARD_LANG_NVS_KEY, out, &n);
        nvs_close(h);
        if (err == ESP_OK && out[0]) return;
    }
    strncpy(out, "en", len - 1);
    out[len - 1] = '\0';
}

esp_err_t board_language_set(const char *lang)
{
    if (!lang || !lang[0]) return ESP_ERR_INVALID_ARG;
    char trimmed[BOARD_LANG_MAX];
    strncpy(trimmed, lang, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(BOARD_NAME_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, BOARD_LANG_NVS_KEY, trimmed);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "phone language set to '%s'", trimmed);
    return err;
}

board_offhook_alert_t board_offhook_alert_get(void)
{
    /* Defaults: on, 120 s. Real switches escalate in stages over ~30-60 s
     * (see the ROH notes in tones.h); 2 minutes is deliberately more patient
     * than a PSTN switch would be -- this is a home phone where sitting on
     * dial tone thinking for a minute is normal, and the alert is loud. */
    board_offhook_alert_t cfg = { .enabled = true, .seconds = 120 };

    nvs_handle_t h;
    if (nvs_open(BOARD_NAME_NVS_NS, NVS_READONLY, &h) != ESP_OK) return cfg;
    uint8_t en;
    if (nvs_get_u8(h, BOARD_ROH_EN_KEY, &en) == ESP_OK) cfg.enabled = (en != 0);
    uint32_t secs;
    if (nvs_get_u32(h, BOARD_ROH_SECS_KEY, &secs) == ESP_OK &&
        secs >= BOARD_OFFHOOK_ALERT_MIN_S && secs <= BOARD_OFFHOOK_ALERT_MAX_S) {
        cfg.seconds = secs;
    }
    nvs_close(h);
    return cfg;
}

esp_err_t board_offhook_alert_set(board_offhook_alert_t cfg)
{
    if (cfg.seconds < BOARD_OFFHOOK_ALERT_MIN_S ||
        cfg.seconds > BOARD_OFFHOOK_ALERT_MAX_S) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOARD_NAME_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, BOARD_ROH_EN_KEY, cfg.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(h, BOARD_ROH_SECS_KEY, cfg.seconds);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "off-hook alert %s, %u s",
                 cfg.enabled ? "enabled" : "disabled", (unsigned)cfg.seconds);
    }
    return err;
}

#define AEC_DELAY_NVS_KEY0 "aec_delay0"
#define AEC_DELAY_NVS_KEY1 "aec_delay1"

static const char *aec_delay_key(int line)
{
    return (line == 1) ? AEC_DELAY_NVS_KEY1 : AEC_DELAY_NVS_KEY0;
}

uint32_t board_aec_delay_get(int line, uint32_t fallback_ms)
{
    nvs_handle_t h;
    if (nvs_open(BOARD_NAME_NVS_NS, NVS_READONLY, &h) != ESP_OK) return fallback_ms;
    uint32_t ms;
    esp_err_t err = nvs_get_u32(h, aec_delay_key(line), &ms);
    nvs_close(h);
    return (err == ESP_OK) ? ms : fallback_ms;
}

esp_err_t board_aec_delay_set(int line, uint32_t ms)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOARD_NAME_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, aec_delay_key(line), ms);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "line %d: AEC delay %u ms persisted", line, (unsigned)ms);
    }
    return err;
}

#define HW_CONFIG_NVS_NS       "board"
#define HW_CONFIG_NVS_KEY_LINES "hw_lines"  /* u8: 1 or 2 */
#define HW_CONFIG_NVS_KEY_SD    "hw_sd"     /* u8: 0 or 1 */

board_hw_config_t board_hw_config_get(void)
{
    /* THT has no GPIO-sharing tradeoff at all (see board_pins_tht.h) --
     * always 2 lines, no microSD, not a runtime question there. */
    if (board_current_type() != BOARD_TYPE_SMD) {
        return (board_hw_config_t){ .num_lines = 2, .has_microsd = false };
    }
    board_hw_config_t cfg = { .num_lines = 1, .has_microsd = false }; /* safe default */
    nvs_handle_t h;
    if (nvs_open(HW_CONFIG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t lines = 0, sd = 0;
        if (nvs_get_u8(h, HW_CONFIG_NVS_KEY_LINES, &lines) == ESP_OK && (lines == 1 || lines == 2)) {
            cfg.num_lines = lines;
        }
        if (nvs_get_u8(h, HW_CONFIG_NVS_KEY_SD, &sd) == ESP_OK) {
            cfg.has_microsd = (sd != 0);
        }
        nvs_close(h);
    }
    return cfg;
}

bool board_hw_config_is_set(void)
{
    if (board_current_type() != BOARD_TYPE_SMD) return true; /* nothing to configure */
    nvs_handle_t h;
    if (nvs_open(HW_CONFIG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t lines = 0;
    esp_err_t err = nvs_get_u8(h, HW_CONFIG_NVS_KEY_LINES, &lines);
    nvs_close(h);
    return err == ESP_OK;
}

esp_err_t board_hw_config_set(board_hw_config_t cfg)
{
    if (cfg.num_lines < 1 || cfg.num_lines > 2) return ESP_ERR_INVALID_ARG;
    if (cfg.num_lines == 2 && cfg.has_microsd) return ESP_ERR_INVALID_ARG; /* GPIO18-21 conflict */

    nvs_handle_t h;
    esp_err_t err = nvs_open(HW_CONFIG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, HW_CONFIG_NVS_KEY_LINES, (uint8_t)cfg.num_lines);
    if (err == ESP_OK) err = nvs_set_u8(h, HW_CONFIG_NVS_KEY_SD, cfg.has_microsd ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "hardware config set: %d line%s, microSD %s",
                 cfg.num_lines, cfg.num_lines == 1 ? "" : "s",
                 cfg.has_microsd ? "yes" : "no");
    }
    return err;
}

board_hw_evidence_t board_hw_detect_evidence(void)
{
    board_hw_evidence_t ev = { .valid = false };
    if (board_current_type() != BOARD_TYPE_SMD) return ev;
    /* GPIO18-21 are only safe to reconfigure as plain inputs here if NO
     * driver has claimed them yet. Once the config is explicitly confirmed
     * (board_hw_config_is_set()), ag1170_init() may already own them as a
     * SLIC (line 2) or sdcard_init() may own them as SPI (microSD) -- in
     * either case, yanking them to GPIO_MODE_INPUT mid-run would glitch a
     * live peripheral (e.g. an in-flight SD SPI transaction). Evidence is
     * only meaningful/safe before that point anyway: it exists to help the
     * user make the FIRST confirmation, not to second-guess one already
     * made. */
    if (board_hw_config_is_set()) return ev;

    const gpio_num_t pins[4] = {
        BOARD_LINE2_PIN_PD_CS, BOARD_LINE2_PIN_SHK_SCLK,
        BOARD_LINE2_PIN_RM_MOSI, BOARD_LINE2_PIN_FR_MISO,
    };
    for (int i = 0; i < 4; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&c);
        ev.raw_levels[i] = gpio_get_level(pins[i]);
    }
    /* An idle/on-hook AG1171 SHK divider (see HARDWARE_SIGNAL_LEVELS.md)
     * pulls this low at rest; an unpopulated/floating microSD CLK pin has no
     * such reason to read consistently low. Weak evidence, not proof --
     * purely advisory, see board.h. */
    ev.looks_like_line2 = (ev.raw_levels[1] == 0);
    ev.valid = true;
    return ev;
}

void board_hostname(char *out, size_t len)
{
    char name[BOARD_NAME_MAX];
    board_name_get(name, sizeof(name));

    /* RFC-1123-ish single label: [a-z0-9-], no leading/trailing '-', never
     * empty (mDNS would reject that). Collapses runs of separators. */
    size_t o = 0;
    bool prev_dash = false;
    for (size_t i = 0; name[i] && o < len - 1; i++) {
        char c = (char)tolower((unsigned char)name[i]);
        if (c == '\'') continue; /* drop apostrophes: "Scott's" -> "scotts" */
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = c;
            prev_dash = false;
        } else if (o > 0 && !prev_dash) {
            out[o++] = '-';
            prev_dash = true;
        }
    }
    while (o > 0 && out[o - 1] == '-') o--; /* trim trailing '-' */
    out[o] = '\0';
    if (out[0] == '\0') {
        strncpy(out, CONFIG_MDNS_HOSTNAME, len - 1);
        out[len - 1] = '\0';
    }
}


#if CONFIG_BOARD_TYPE_SMD2
#define LED_TICK_MS   100  /* fastest blink rate (BOOTING); ERROR blinks
                             * slower by only toggling every few ticks */
#define LED_DUTY_MAX  1023 /* 10-bit LEDC resolution */
#define LED_TIMER     LEDC_TIMER_0
#define LED_CHANNEL   LEDC_CHANNEL_0
#define LED_SPEED     LEDC_LOW_SPEED_MODE /* C6 has no high-speed LEDC mode */

static esp_timer_handle_t   s_led_timer;
static board_led_state_t    s_led_state = BOARD_LED_BOOTING;

static void led_set_duty(uint32_t duty)
{
    ledc_set_duty(LED_SPEED, LED_CHANNEL, duty);
    ledc_update_duty(LED_SPEED, LED_CHANNEL);
}

static void led_timer_cb(void *arg)
{
    static unsigned tick;
    tick++;
    switch (s_led_state) {
    case BOARD_LED_OK:
        led_set_duty(0);
        break;
    case BOARD_LED_ERROR:
        led_set_duty(((tick / 5) % 2) ? LED_DUTY_MAX : 0); /* ~500ms half-period */
        break;
    case BOARD_LED_WIFI_CONNECTING: {
        /* Slow linear ramp up/down (a triangle wave, not a true sine, but
         * reads as "breathing" at this rate and needs no lookup table) --
         * full cycle every 3s (30 ticks at 100ms/tick). */
        const unsigned period = 30, half = period / 2;
        unsigned phase = tick % period;
        uint32_t duty = (phase < half) ? (phase * LED_DUTY_MAX) / half
                                        : LED_DUTY_MAX - ((phase - half) * LED_DUTY_MAX) / half;
        led_set_duty(duty);
        break;
    }
    case BOARD_LED_BOOTING:
    default:
        led_set_duty((tick % 2) ? LED_DUTY_MAX : 0); /* ~100ms half-period */
        break;
    }
}

void board_led_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LED_SPEED,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LED_TIMER,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num = BOARD_STATUS_LED,
        .speed_mode = LED_SPEED,
        .channel = LED_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));

    const esp_timer_create_args_t targs = {
        .callback = &led_timer_cb,
        .name = "status_led",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_led_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_led_timer, LED_TICK_MS * 1000));
}

void board_led_set_state(board_led_state_t state)
{
    s_led_state = state;
}
#else
void board_led_init(void) { }
void board_led_set_state(board_led_state_t state) { (void)state; }
#endif

/* ---- BOOT-strap (GPIO9) factory-reset watcher ----
 * GPIO9 is a boot strap: held low AT reset the ROM enters serial-download
 * mode and our app never runs. So this only ever reads it *after* boot -- a
 * runtime hold, not a power-on gesture. Free on both PCBs (not in either pin
 * map). */
#define FACTORY_RESET_GPIO     GPIO_NUM_9
#define FACTORY_RESET_HOLD_MS  3000
#define FACTORY_RESET_POLL_MS  100

static void (*s_factory_reset_cb)(void);

static void factory_reset_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FACTORY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* idle high; short to GND to trigger */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    while (1) {
        if (gpio_get_level(FACTORY_RESET_GPIO) == 0) {
            held_ms += FACTORY_RESET_POLL_MS;
            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "GPIO%d (BOOT) held %d ms -- factory reset",
                         FACTORY_RESET_GPIO, held_ms);
                if (s_factory_reset_cb) s_factory_reset_cb();
                held_ms = 0; /* if the cb didn't reboot, don't re-fire every tick */
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_POLL_MS));
    }
}

void board_factory_reset_start(void (*on_hold)(void))
{
    if (!on_hold) return;
    s_factory_reset_cb = on_hold;
    xTaskCreate(factory_reset_task, "factory_rst", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
}
