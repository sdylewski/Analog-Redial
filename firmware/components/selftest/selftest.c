#include "selftest.h"
#include "board.h"
#include "board_pins.h"
#include "ag1170.h"
#include "es8388.h"
#include "bt_companion.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "selftest";

#define SELFTEST_NVS_NS  "selftest"
#define HISTORY_NVS_KEY  "hist"

/* Before this, the clock has obviously never synced (2020-09-13). Stored as 0
 * rather than a bogus 1970 date, so the UI can honestly say "date unknown"
 * instead of showing a wrong one. */
#define CLOCK_SANE_AFTER 1600000000LL

static selftest_report_t s_report;

/* ---- report plumbing --------------------------------------------------- */

static selftest_check_t *find_check(const char *id)
{
    for (int i = 0; i < s_report.n; i++) {
        if (strcmp(s_report.checks[i].id, id) == 0) return &s_report.checks[i];
    }
    return NULL;
}

static selftest_check_t *add_check(const char *id, const char *title,
                                   const char *instruction, selftest_kind_t kind)
{
    if (s_report.n >= SELFTEST_MAX_CHECKS) return NULL;
    selftest_check_t *c = &s_report.checks[s_report.n++];
    memset(c, 0, sizeof(*c));
    snprintf(c->id, sizeof(c->id), "%s", id);
    c->title = title;
    c->instruction = instruction;
    c->kind = kind;
    c->result = SELFTEST_PENDING;
    return c;
}

static void set_result(selftest_check_t *c, selftest_result_t r, const char *fmt, ...)
{
    if (!c) return;
    c->result = r;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
        va_end(ap);
    } else {
        c->detail[0] = '\0';
    }
}

/* ---- individual automated checks --------------------------------------- */

static void check_codec(selftest_check_t *c)
{
    uint8_t v = 0;
    if (es8388_read_reg(ES8388_REG_CONTROL1, &v) != ESP_OK) {
        set_result(c, SELFTEST_FAIL, "no I2C reply at 0x%02X", BOARD_CODEC_I2C_ADDR);
        return;
    }
    set_result(c, SELFTEST_PASS, "ES8388 answering at 0x%02X", BOARD_CODEC_I2C_ADDR);
}

/* Catches the class of fault that made dial tone inaudible for most of
 * 2026-09-26: the output path silently muted or attenuated at the codec,
 * which looks exactly like a dead SLIC from the handset. */
static void check_codec_cfg(selftest_check_t *c)
{
    uint8_t dacpower = 0, lout1 = 0, ldacvol = 0;
    if (es8388_read_reg(ES8388_REG_DACPOWER, &dacpower) != ESP_OK ||
        es8388_read_reg(ES8388_REG_LOUT1VOL, &lout1) != ESP_OK ||
        es8388_read_reg(ES8388_REG_LDACVOL, &ldacvol) != ESP_OK) {
        set_result(c, SELFTEST_FAIL, "register readback failed");
        return;
    }
    /* DACPOWER: bit7 PdnDACL / bit6 PdnDACR are power-DOWN (want 0),
     * bit5 LOUT1 / bit4 ROUT1 are output enables (want 1). */
    if (dacpower & 0x80) {
        set_result(c, SELFTEST_FAIL, "left DAC powered down (DACPOWER=0x%02X)", dacpower);
        return;
    }
    if (!(dacpower & 0x20)) {
        set_result(c, SELFTEST_FAIL, "LOUT1 disabled (DACPOWER=0x%02X)", dacpower);
        return;
    }
    if (board_hw_config_get().num_lines >= 2 && !(dacpower & 0x10)) {
        set_result(c, SELFTEST_FAIL, "ROUT1 disabled but line 1 exists (0x%02X)", dacpower);
        return;
    }
    /* Datasheet Reg. 46: 0=-45dB, 30=0dB, 33=+4.5dB is the documented top --
     * anything above 33 is undefined and measured as dead silence. */
    if (lout1 > 33) {
        set_result(c, SELFTEST_FAIL, "LOUT1VOL=%u is past the documented max of 33", lout1);
        return;
    }
    if (lout1 < 20) {
        set_result(c, SELFTEST_FAIL, "LOUT1VOL=%u is very quiet (30 = 0dB)", lout1);
        return;
    }
    if (ldacvol != 0) {
        set_result(c, SELFTEST_FAIL, "digital DAC gain attenuated (LDACVOL=0x%02X)", ldacvol);
        return;
    }
    set_result(c, SELFTEST_PASS, "LOUT1VOL=%u, outputs enabled, 0dB digital", lout1);
}

/* An unconfirmed board variant is a FAIL for bring-up purposes, not a
 * warning: on 2026-09-26 a 2-line board carried an auto-guessed "1 line +
 * microSD" config, so line 1 was never initialized while the SD driver drove
 * its SLIC control pins as SPI. Nothing else in the test means much until
 * somebody has confirmed what this board actually is. */
static void check_board_cfg(selftest_check_t *c)
{
    board_hw_config_t cfg = board_hw_config_get();
    if (!board_hw_config_is_set()) {
        set_result(c, SELFTEST_FAIL,
                   "variant never confirmed (guessed %d-line%s)",
                   cfg.num_lines, cfg.has_microsd ? " + SD" : "");
        return;
    }
    set_result(c, SELFTEST_PASS, "confirmed %d-line%s",
               cfg.num_lines, cfg.has_microsd ? " + microSD" : ", no microSD");
}

static void check_sd_conflict(selftest_check_t *c)
{
    if (board_sd_line1_conflict()) {
        set_result(c, SELFTEST_FAIL,
                   "microSD card inserted on a 2-line board -- line 1 is disabled");
        return;
    }
    set_result(c, SELFTEST_PASS, "no microSD/line-1 conflict");
}

/* Only meaningful once a module is declared fitted (Settings -> Hardware) --
 * which is exactly the distinction that was missing on 2026-09-26, when the
 * feature being merely "enabled" with no hardware attached claimed a phone
 * line and killed dial tone. */
static void check_bt_link(selftest_check_t *c)
{
    if (!bt_companion_enabled()) {
        set_result(c, SELFTEST_FAIL, "module fitted but the feature is switched off");
        return;
    }
    bt_companion_status_t st = bt_companion_get_status();
    if (!st.link_up) {
        set_result(c, SELFTEST_FAIL, "no reply over UART -- check GPIO16/17 (TX/RX crossed)");
        return;
    }
    set_result(c, SELFTEST_PASS, "module responding%s%s", st.bd_addr[0] ? ", addr " : "",
               st.bd_addr[0] ? st.bd_addr : "");
}

/* A stock BM64 routes audio to I2S, leaving the analog pin this board taps
 * silent no matter how loud the phone is -- the fault that cost most of
 * 2026-09-26. bt_companion_config_check() records the module's BD address
 * once its EEPROM is right, so this is a cheap read of that marker rather
 * than a 15s IBDK round trip. */
static void check_bt_eeprom(selftest_check_t *c)
{
    if (!bt_companion_get_status().link_up) {
        set_result(c, SELFTEST_SKIP, "module not responding, can't tell");
        return;
    }
    if (!bt_companion_config_is_verified()) {
        set_result(c, SELFTEST_FAIL,
                   "EEPROM not verified -- run the check in Settings -> Bluetooth");
        return;
    }
    set_result(c, SELFTEST_PASS, "audio EEPROM bytes verified for this module");
}

/* ---- building the check list ------------------------------------------- */

void selftest_begin(void)
{
    memset(&s_report, 0, sizeof(s_report));
    int lines = board_hw_config_get().num_lines;

    check_board_cfg(add_check("board", "Board variant", NULL, SELFTEST_AUTO));
    check_sd_conflict(add_check("sd", "microSD / line-1 conflict", NULL, SELFTEST_AUTO));
    check_codec(add_check("codec", "Audio codec on I2C", NULL, SELFTEST_AUTO));
    check_codec_cfg(add_check("codeccfg", "Codec output configuration", NULL, SELFTEST_AUTO));

    /* Hook and audio are deliberately separate per line -- they fail
     * independently (see this file's header comment). */
    add_check("hook0", "Line 0 hook detection",
              "Lift the line 0 handset (J9), then put it back down.",
              SELFTEST_ACTION);
    add_check("dial0", "Line 0 dial tone",
              "Lift the line 0 handset and listen. Do you hear dial tone?",
              SELFTEST_CONFIRM);
    add_check("ring0", "Line 0 ringer",
              "The line 0 phone should ring for a few seconds. Did it ring?",
              SELFTEST_CONFIRM);

    selftest_check_t *h1 = add_check("hook1", "Line 1 hook detection",
                                      "Lift the line 1 handset (J8), then put it back down.",
                                      SELFTEST_ACTION);
    selftest_check_t *d1 = add_check("dial1", "Line 1 dial tone",
                                      "Lift the line 1 handset and listen. Do you hear dial tone?",
                                      SELFTEST_CONFIRM);
    selftest_check_t *r1 = add_check("ring1", "Line 1 ringer",
                                      "The line 1 phone should ring for a few seconds. Did it ring?",
                                      SELFTEST_CONFIRM);
    if (lines < 2) {
        set_result(h1, SELFTEST_SKIP, "board has one line");
        set_result(d1, SELFTEST_SKIP, "board has one line");
        set_result(r1, SELFTEST_SKIP, "board has one line");
    }

    selftest_check_t *bl = add_check("btlink", "Bluetooth module link", NULL, SELFTEST_AUTO);
    selftest_check_t *be = add_check("bteeprom", "Bluetooth audio EEPROM", NULL, SELFTEST_AUTO);
    if (board_hw_config_get().has_bt_module) {
        check_bt_link(bl);
        check_bt_eeprom(be);
    } else {
        set_result(bl, SELFTEST_SKIP, "no BM64 module fitted");
        set_result(be, SELFTEST_SKIP, "no BM64 module fitted");
    }

}

const selftest_report_t *selftest_report(void)
{
    return &s_report;
}

/* ---- human-driven steps ------------------------------------------------ */

static int line_from_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0) return -1;
    char last = id[n - 1];
    if (last == '0') return 0;
    if (last == '1') return 1;
    return -1;
}

selftest_result_t selftest_run_action(const char *id, int timeout_ms)
{
    selftest_check_t *c = find_check(id);
    if (!c) return SELFTEST_FAIL;
    if (c->result == SELFTEST_SKIP) return SELFTEST_SKIP;

    int line = line_from_id(id);
    if (line < 0 || line >= board_hw_config_get().num_lines) {
        set_result(c, SELFTEST_SKIP, "line not present on this board");
        return SELFTEST_SKIP;
    }

    /* Read SHK straight from the pin, not through telephony's state machine
     * or ag1170's debounce: this check exists to prove the physical hook
     * signal reaches the GPIO at all. A bad J5 jumper (2026-09-26) shows up
     * here as "never changed" while every audio test on that line passes. */
    int start = ag1170_raw_shk(line);
    if (start < 0) {
        set_result(c, SELFTEST_FAIL, "line not serviced by the SLIC driver");
        return SELFTEST_FAIL;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        int now = ag1170_raw_shk(line);
        if (now != start) {
            set_result(c, SELFTEST_PASS, "SHK changed %d -> %d", start, now);
            return SELFTEST_PASS;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    set_result(c, SELFTEST_FAIL,
               "SHK stayed at %d for %ds -- check the SHK wiring/jumper",
               start, timeout_ms / 1000);
    return SELFTEST_FAIL;
}

esp_err_t selftest_stimulus(const char *id)
{
    selftest_check_t *c = find_check(id);
    if (!c) return ESP_ERR_NOT_FOUND;
    if (c->result == SELFTEST_SKIP) return ESP_ERR_INVALID_STATE;

    /* Only the ringer needs firmware to produce anything -- the dial-tone
     * check's stimulus is the human lifting the handset. */
    if (strncmp(id, "ring", 4) == 0) {
        int line = line_from_id(id);
        if (line < 0) return ESP_ERR_INVALID_ARG;
        ESP_LOGI(TAG, "ringing line %d for the ringer check", line);
        ag1170_ring_start(line);
        vTaskDelay(pdMS_TO_TICKS(4000));
        ag1170_ring_stop(line);
    }
    return ESP_OK;
}

/* ---- background runner (web UI) ---------------------------------------- */

static volatile bool s_busy;
static char s_job_id[SELFTEST_ID_MAX];
static int s_job_timeout_ms;
static bool s_job_stimulus;

static void bg_task(void *arg)
{
    if (s_job_stimulus) selftest_stimulus(s_job_id);
    else selftest_run_action(s_job_id, s_job_timeout_ms);
    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t start_bg(const char *id, int timeout_ms, bool stimulus)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    selftest_check_t *c = find_check(id);
    if (!c) return ESP_ERR_NOT_FOUND;
    if (c->result == SELFTEST_SKIP) return ESP_ERR_INVALID_STATE;
    strlcpy(s_job_id, id, sizeof(s_job_id));
    s_job_timeout_ms = timeout_ms;
    s_job_stimulus = stimulus;
    if (!stimulus) set_result(c, SELFTEST_PENDING, "watching for %ds...", timeout_ms / 1000);
    s_busy = true;
    if (xTaskCreate(bg_task, "selftest_bg", 4096, NULL, 5, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t selftest_run_action_async(const char *id, int timeout_ms)
{
    return start_bg(id, timeout_ms, false);
}

esp_err_t selftest_stimulus_async(const char *id)
{
    return start_bg(id, 0, true);
}

bool selftest_busy(void)
{
    return s_busy;
}

esp_err_t selftest_answer(const char *id, bool ok)
{
    selftest_check_t *c = find_check(id);
    if (!c) return ESP_ERR_NOT_FOUND;
    if (c->result == SELFTEST_SKIP) return ESP_ERR_INVALID_STATE;
    set_result(c, ok ? SELFTEST_PASS : SELFTEST_FAIL, ok ? "confirmed by operator"
                                                         : "operator reported a problem");
    return ESP_OK;
}

/* ---- identity + history ------------------------------------------------ */

void selftest_board_id(char *out, size_t len)
{
    /* Delegates rather than re-deriving: board_serial_str() already formats
     * this board's factory MAC, and two implementations of "which board is
     * this" would be one too many. */
    board_serial_str(out, len);
}

size_t selftest_history_get(selftest_run_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    nvs_handle_t h;
    if (nvs_open(SELFTEST_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    selftest_run_t stored[SELFTEST_HISTORY_MAX];
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, HISTORY_NVS_KEY, stored, &size);
    nvs_close(h);
    if (err != ESP_OK) return 0;
    size_t n = size / sizeof(selftest_run_t);
    if (n > max) n = max;
    memcpy(out, stored, n * sizeof(selftest_run_t));
    return n;
}

esp_err_t selftest_history_append(void)
{
    selftest_run_t entry = { 0 };
    for (int i = 0; i < s_report.n; i++) {
        switch (s_report.checks[i].result) {
            case SELFTEST_PASS:    entry.n_pass++; break;
            case SELFTEST_FAIL:    entry.n_fail++; break;
            case SELFTEST_SKIP:    entry.n_skip++; break;
            default:               entry.n_pending++; break;
        }
    }
    time_t now = time(NULL);
    entry.unix_time = ((int64_t)now > CLOCK_SANE_AFTER) ? (int64_t)now : 0;

    /* Newest first, oldest falls off the end. */
    selftest_run_t hist[SELFTEST_HISTORY_MAX];
    size_t have = selftest_history_get(hist, SELFTEST_HISTORY_MAX);
    size_t keep = (have < SELFTEST_HISTORY_MAX - 1) ? have : SELFTEST_HISTORY_MAX - 1;
    selftest_run_t updated[SELFTEST_HISTORY_MAX];
    updated[0] = entry;
    memcpy(&updated[1], hist, keep * sizeof(selftest_run_t));

    nvs_handle_t h;
    esp_err_t err = nvs_open(SELFTEST_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, HISTORY_NVS_KEY, updated, (keep + 1) * sizeof(selftest_run_t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "recorded run: %u pass, %u fail, %u skip, %u unanswered",
                 entry.n_pass, entry.n_fail, entry.n_skip, entry.n_pending);
    }
    return err;
}
