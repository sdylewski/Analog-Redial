#include "bringup.h"
#include "board.h"
#include "board_pins.h"
#include "board_config.h"
#if CONFIG_BOARD_TYPE_SMD2
#include "es8388.h"
#else
#include "wm8731.h"
#endif
#include "audio.h"
#include "ag1170.h"
#include "dtmf.h"
#include "telephony.h"
#include "wifi.h"
#include "storage.h"
#include "player.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "bringup";

/* ---------- i2c_scan ---------- */
static int cmd_board_info(int argc, char **argv)
{
    printf("board: %s\n", board_type_name(board_current_type()));
    printf("lines: %d\n", BOARD_NUM_LINES);
    printf("i2c: SDA=%d SCL=%d\n", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return 0;
}

static int cmd_i2c_scan(int argc, char **argv)
{
    if (board_i2c_init() != ESP_OK) {
        printf("I2C init failed\n");
        return 1;
    }
    i2c_master_bus_handle_t bus = board_i2c_bus();
    printf("Scanning I2C bus (SDA=%d SCL=%d)...\n", BOARD_I2C_SDA, BOARD_I2C_SCL);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            printf("  found 0x%02X%s\n", addr,
                   addr == BOARD_CODEC_I2C_ADDR ? "  <- codec" : "");
            found++;
        }
    }
    printf("%d device(s) found%s\n", found,
           found == 0 ? " (expected if no PCB / codec not powered)" : "");
    return 0;
}

static int cmd_codec_detect(int argc, char **argv)
{
    board_detect_codec();
    printf("see log output above (also runs automatically at boot)\n");
    return 0;
}

/* ---------- codec_init / dump / bypass (ES8388 on SMD, WM8731 on THT) ---------- */
static int cmd_codec_init(int argc, char **argv)
{
#if CONFIG_BOARD_TYPE_SMD2
    es8388_config_t c = ES8388_DEFAULT_CONFIG();
    c.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    esp_err_t err = es8388_init(&c);
#else
    wm8731_config_t c = WM8731_DEFAULT_CONFIG();
    c.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    esp_err_t err = wm8731_init(&c);
#endif
    printf("codec_init: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_codec_dump(int argc, char **argv)
{
#if CONFIG_BOARD_TYPE_SMD2
    static const struct { uint8_t reg; const char *name; } regs[] = {
        { ES8388_REG_CONTROL1, "CONTROL1" }, { ES8388_REG_CONTROL2, "CONTROL2" },
        { ES8388_REG_CHIPPOWER, "CHIPPOWER" }, { ES8388_REG_ADCPOWER, "ADCPOWER" },
        { ES8388_REG_DACPOWER, "DACPOWER" }, { ES8388_REG_MASTERMODE, "MASTERMODE" },
        { ES8388_REG_ADCCONTROL1, "ADCCONTROL1" }, { ES8388_REG_ADCCONTROL2, "ADCCONTROL2" },
        { ES8388_REG_LADCVOL, "LADCVOL" }, { ES8388_REG_RADCVOL, "RADCVOL" },
        { ES8388_REG_DACCONTROL1, "DACCONTROL1" }, { ES8388_REG_LDACVOL, "LDACVOL" },
        { ES8388_REG_RDACVOL, "RDACVOL" }, { ES8388_REG_DACCONTROL16, "DACCONTROL16" },
        { ES8388_REG_DACCONTROL17, "DACCONTROL17" }, { ES8388_REG_DACCONTROL20, "DACCONTROL20" },
        { ES8388_REG_DACCONTROL21, "DACCONTROL21" }, { ES8388_REG_LOUT1VOL, "LOUT1VOL" },
        { ES8388_REG_ROUT1VOL, "ROUT1VOL" }, { ES8388_REG_LOUT2VOL, "LOUT2VOL" },
        { ES8388_REG_ROUT2VOL, "ROUT2VOL" },
    };
    printf("ES8388 registers: shadow (what we wrote) vs. actual I2C readback (what's really latched)\n");
    printf("  %-4s %-13s %-8s %-8s\n", "reg", "name", "shadow", "actual");
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t actual = 0;
        esp_err_t err = es8388_read_reg(regs[i].reg, &actual);
        uint8_t shadow = es8388_shadow(regs[i].reg);
        if (err != ESP_OK) {
            printf("  0x%02X %-13s 0x%02X     read failed: %s\n",
                   regs[i].reg, regs[i].name, shadow, esp_err_to_name(err));
        } else {
            printf("  0x%02X %-13s 0x%02X     0x%02X%s\n",
                   regs[i].reg, regs[i].name, shadow, actual,
                   (actual != shadow) ? "   <-- MISMATCH" : "");
        }
    }
#else
    const char *names[] = {
        "LLINEIN", "RLINEIN", "LHPOUT", "RHPOUT", "ANALOG", "DIGITAL",
        "POWER", "IFACE", "SAMPLING", "ACTIVE"
    };
    printf("WM8731 shadow registers (write-only device):\n");
    for (int i = 0; i < 10; i++) {
        printf("  R%-2d %-9s = 0x%03X\n", i, names[i], wm8731_shadow(i));
    }
#endif
    return 0;
}

static int cmd_codec_bypass(int argc, char **argv)
{
#if CONFIG_BOARD_TYPE_SMD2
    printf("analog bypass: not supported on ES8388 in this driver -- use "
           "'loopback' for a DSP-side (I2S) loopback instead\n");
    return 1;
#else
    bool on = (argc > 1 && atoi(argv[1]) != 0);
    printf("analog bypass %s: %s\n", on ? "ON" : "OFF",
           esp_err_to_name(wm8731_set_bypass(on)));
    return 0;
#endif
}

/* ---------- micamp (ES8388 ADC PGA gain, live tuning) ---------- */
static int cmd_micamp(int argc, char **argv)
{
#if CONFIG_BOARD_TYPE_SMD2
    if (argc < 3) {
        printf("usage: micamp <left 0-8> <right 0-8>   (0=0dB ... 8=+24dB, 3dB/step)\n"
               "current ADCCONTROL1 (0x09) = 0x%02X\n", es8388_shadow(ES8388_REG_ADCCONTROL1));
        return 1;
    }
    int l = atoi(argv[1]), r = atoi(argv[2]);
    if (l < 0 || l > 8 || r < 0 || r > 8) {
        printf("left/right must each be 0-8\n");
        return 1;
    }
    uint8_t val = (uint8_t)((l << 4) | r);
    esp_err_t err = es8388_write_reg(ES8388_REG_ADCCONTROL1, val);
    if (err != ESP_OK) {
        printf("write failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("MicAmpL=+%ddB  MicAmpR=+%ddB  (ADCCONTROL1=0x%02X)\n", l * 3, r * 3, val);
    return 0;
#else
    printf("micamp: SMD2/ES8388 only\n");
    return 1;
#endif
}

/* ---------- tone ---------- */
static struct {
    struct arg_dbl *hz;
    struct arg_int *ms;
    struct arg_int *line;
    struct arg_end *end;
} tone_args;

static int cmd_tone(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&tone_args) != 0) {
        arg_print_errors(stdout, tone_args.end, "tone");
        return 1;
    }
    double hz = tone_args.hz->count ? tone_args.hz->dval[0] : 1000.0;
    int ms = tone_args.ms->count ? tone_args.ms->ival[0] : 1000;
    int line = tone_args.line->count ? tone_args.line->ival[0] : 0;
    printf("line %d: %.0f Hz for %d ms\n", line, hz, ms);
    audio_tx_burst(line, (float)hz, 0.0f, 0.6f, ms);
    return 0;
}

/* ---------- call-progress tones ---------- */
static int cmd_progress(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    call_progress_t t = TONE_DIAL;
    if (strcmp(argv[0], "ringback") == 0) t = TONE_RINGBACK;
    else if (strcmp(argv[0], "busy") == 0) t = TONE_BUSY;
    printf("line %d: %s\n", line, argv[0]);
    audio_tx_tone(line, t);
    return 0;
}

static int cmd_silence(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    audio_tx_silence(line);
    printf("line %d: silence\n", line);
    return 0;
}

/* ---------- loopback (mic -> own speaker) ---------- */
static int cmd_loopback(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    bool on = (argc > 2) ? (atoi(argv[2]) != 0) : true;
    audio_tx_bridge(line, line, on);
    printf("line %d loopback %s\n", line, on ? "ON" : "OFF");
    return 0;
}

/* ---------- capture (RX level meter) ---------- */
static int cmd_capture(int argc, char **argv)
{
    int ms = (argc > 1) ? atoi(argv[1]) : 3000;
    if (argc > 2) {
        printf("both lines capture simultaneously now -- line arg ignored\n");
    }
    int steps = ms / 200;
    for (int i = 0; i < steps; i++) {
        printf("  L0 rms=%5u   L1 rms=%5u\n",
               audio_rx_level(0), audio_rx_level(1));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return 0;
}

/* ---------- record (raw mic capture -> storage, no live bridge/playback) ---------- */
static void wav_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void wav_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}

/* Captures a line's raw ADC audio straight from audio_rx_read() -- the same
 * stream buffer the VoIP seam would pull from -- entirely independent of
 * audio_tx_bridge()/telephony. Nothing gets played back to any speaker, so
 * this is safe to use mid-call to inspect one direction in isolation
 * (e.g. is line1's capture itself noisy, regardless of what the bridge
 * does with it). Saved as a standard 44-byte-header mono 16-bit PCM WAV to
 * the existing storage layer -- downloadable from the web UI's /recordings
 * page or GET /media/<name>. */
static int cmd_record(int argc, char **argv)
{
    if (argc < 2) { printf("usage: record <ms> [line]\n"); return 1; }
    int ms = atoi(argv[1]);
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    if (ms <= 0 || line < 0 || line > 1) { printf("bad args\n"); return 1; }

    uint32_t sr = audio_sample_rate();
    size_t n_samples = (size_t)((int64_t)sr * ms / 1000);
    size_t wav_bytes = 44 + n_samples * sizeof(int16_t);

    /* Single-shot malloc for the whole clip -- keep it within what's
     * actually free, capping duration rather than failing outright. */
    size_t free_heap = esp_get_free_heap_size();
    size_t safety = 32768; /* headroom for other tasks/allocs while we run */
    size_t max_bytes = (free_heap > safety) ? (free_heap - safety) : 0;
    if (wav_bytes > max_bytes) {
        size_t max_samples = (max_bytes > 44) ? (max_bytes - 44) / sizeof(int16_t) : 0;
        if (max_samples == 0) { printf("not enough free heap to record anything\n"); return 1; }
        printf("%d ms exceeds free heap -- capping to %u ms\n",
               ms, (unsigned)((uint64_t)max_samples * 1000 / sr));
        n_samples = max_samples;
        wav_bytes = 44 + n_samples * sizeof(int16_t);
    }

    uint8_t *wav = malloc(wav_bytes);
    if (!wav) { printf("malloc(%u) failed\n", (unsigned)wav_bytes); return 1; }

    size_t data_bytes = n_samples * sizeof(int16_t);
    memcpy(wav, "RIFF", 4);
    wav_put_u32(wav + 4, 36 + (uint32_t)data_bytes);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    wav_put_u32(wav + 16, 16);
    wav_put_u16(wav + 20, 1);            /* PCM */
    wav_put_u16(wav + 22, 1);            /* mono */
    wav_put_u32(wav + 24, sr);
    wav_put_u32(wav + 28, sr * 2);       /* byte rate: sr * channels * bytes/sample */
    wav_put_u16(wav + 32, 2);            /* block align */
    wav_put_u16(wav + 34, 16);           /* bits per sample */
    memcpy(wav + 36, "data", 4);
    wav_put_u32(wav + 40, (uint32_t)data_bytes);

    printf("recording line %d for %u ms (%u samples)...\n",
           line, (unsigned)((uint64_t)n_samples * 1000 / sr), (unsigned)n_samples);
    int16_t *pcm = (int16_t *)(wav + 44);
    size_t got = 0;
    while (got < n_samples) {
        size_t chunk = audio_rx_read(line, pcm + got, n_samples - got, 2000);
        if (chunk == 0) { printf("audio_rx_read timed out after %u/%u samples\n",
                                  (unsigned)got, (unsigned)n_samples); break; }
        got += chunk;
    }

    char name[STORAGE_MAX_NAME];
    snprintf(name, sizeof(name), "rec_l%d_%lld.wav", line, (long long)(esp_timer_get_time() / 1000));
    size_t final_bytes = 44 + got * sizeof(int16_t);
    wav_put_u32(wav + 4, 36 + (uint32_t)(got * sizeof(int16_t)));
    wav_put_u32(wav + 40, (uint32_t)(got * sizeof(int16_t)));
    esp_err_t err = storage_write(name, wav, final_bytes);
    free(wav);
    if (err != ESP_OK) {
        printf("storage_write failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved %s (%u bytes) -- download at /media/%s or the web UI's Recordings tab\n",
           name, (unsigned)final_bytes, name);
    return 0;
}

/* ---------- play (test the player component standalone) ---------- */
static int cmd_play(int argc, char **argv)
{
    if (argc < 2) { printf("usage: play <storage name> [line]\n"); return 1; }
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    esp_err_t err = player_play(line, argv[1], NULL, NULL);
    if (err != ESP_OK) {
        printf("player_play failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("playing '%s' on line %d...\n", argv[1], line);
    return 0;
}

/* ---------- dtmf_gen ---------- */
static int cmd_dtmf_gen(int argc, char **argv)
{
    if (argc < 2) { printf("usage: dtmf_gen <digits> [line]\n"); return 1; }
    const char *digits = argv[1];
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    for (const char *p = digits; *p; p++) {
        printf("  '%c'\n", *p);
        audio_tx_dtmf(line, *p, 150);
        vTaskDelay(pdMS_TO_TICKS(220)); /* 150 tone + ~70 gap */
    }
    audio_tx_silence(line);
    return 0;
}

/* ---------- slic raw control ---------- */
static int cmd_slic(int argc, char **argv)
{
    if (argc < 4) { printf("usage: slic <line> <pd|fr|rm> <0|1>\n"); return 1; }
    int line = atoi(argv[1]);
    int val = atoi(argv[3]);
    if (strcmp(argv[2], "pd") == 0)      ag1170_set_power(line, val != 0);
    else if (strcmp(argv[2], "fr") == 0) ag1170_set_fr(line, val);
    else if (strcmp(argv[2], "rm") == 0) ag1170_set_rm(line, val);
    else { printf("unknown pin %s\n", argv[2]); return 1; }
    printf("line %d %s = %d\n", line, argv[2], val);
    return 0;
}

/* ---------- ring ---------- */
static int cmd_ring(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    int secs = (argc > 2) ? atoi(argv[2]) : 6;
    printf("line %d ring for %d s\n", line, secs);
    ag1170_ring_start(line);
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    ag1170_ring_stop(line);
    printf("ring done\n");
    return 0;
}

/* ---------- hookmon ---------- */
static int cmd_hookmon(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 10;
    printf("Monitoring SHK for %d s (lift/replace handsets, dial)...\n", secs);
    int prev0 = -1, prev1 = -1;
    for (int i = 0; i < secs * 50; i++) {
        int r0 = ag1170_raw_shk(0), r1 = ag1170_raw_shk(1);
        if (r0 != prev0 || r1 != prev1) {
            printf("  raw SHK: L0=%d(%s) L1=%d(%s)\n",
                   r0, ag1170_is_offhook(0) ? "off" : "on",
                   r1, ag1170_is_offhook(1) ? "off" : "on");
            prev0 = r0; prev1 = r1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 0;
}

/* ---------- pulsemon ---------- */
static int cmd_pulsemon(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 15;
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    printf("Monitoring line %d pulse dialing for %d s (dial digits now)...\n", line, secs);
    printf("Non-invasive poll -- doesn't touch the AG1170_EV_DIGIT callback telephony uses,\n"
           "so normal dialing/calls keep working while this runs.\n");

    int64_t t0 = esp_timer_get_time();
    int64_t last_edge_us = t0;
    int prev_raw = -1, prev_count = -1;
    bool prev_counting = false;

    for (int i = 0; i < secs * 200; i++) {  /* 5 ms steps -- pulses run ~10/s */
        int64_t now = esp_timer_get_time();
        long long t_ms = (long long)((now - t0) / 1000);
        int raw = ag1170_raw_shk(line);
        int count = ag1170_pulse_count(line);
        bool counting = ag1170_is_counting_digit(line);

        if (raw != prev_raw) {
            printf("  t=%6lldms  SHK -> %d   (%lldms since last edge)\n",
                   t_ms, raw, (long long)((now - last_edge_us) / 1000));
            last_edge_us = now;
            prev_raw = raw;
        }
        if (prev_counting && !counting) {
            /* pulse_count already reset to 0 by the time counting_digit
             * flips false -- prev_count still holds the final tally. */
            int digit = prev_count % 10; /* 10 pulses -> 0, matches ag1170.c */
            printf("  t=%6lldms  DIGIT COMPLETE: %d pulses -> '%c'\n",
                   t_ms, prev_count, (char)((digit == 0) ? '0' : '0' + digit));
        } else if (count != prev_count) {
            printf("  t=%6lldms  pulse_count=%d\n", t_ms, count);
        }
        prev_count = count;
        prev_counting = counting;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

/* ---------- tonemon ---------- */
static int cmd_tonemon(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 15;
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    printf("Monitoring line %d for DTMF (touch-tone) digits for %d s (dial digits now, or "
           "run 'dtmf_gen <digits> %d' from another session)...\n", line, secs, line);
    printf("Non-invasive poll -- reads telephony's own always-running Goertzel detector,\n"
           "doesn't touch the RX tap, so normal dialing/calls keep working while this runs.\n");

    int64_t t0 = esp_timer_get_time();
    int64_t prev_seen_us = -1;
    int64_t last_digit_us = t0;

    for (int i = 0; i < secs * 200; i++) {  /* 5 ms steps */
        int64_t at_us = 0;
        char d = telephony_last_dtmf(line, &at_us);
        if (d && at_us != prev_seen_us) {
            int64_t now = esp_timer_get_time();
            printf("  t=%6lldms  DTMF '%c'   (%lldms since last digit)\n",
                   (long long)((now - t0) / 1000), d,
                   (long long)((now - last_digit_us) / 1000));
            last_digit_us = now;
            prev_seen_us = at_us;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

/* ---------- status ---------- */
static int cmd_status(int argc, char **argv)
{
    for (int l = 0; l < 2; l++) {
        printf("line %d: state=%s offhook=%d ringing=%d last=%s\n",
               l, telephony_state_name(telephony_state(l)),
               ag1170_is_offhook(l), ag1170_is_ringing(l),
               telephony_last_number(l));
    }
    return 0;
}

/* ---------- reset ---------- */
static int cmd_reset(int argc, char **argv)
{
    if (argc < 2) {
        for (int l = 0; l < 2; l++) telephony_force_recover(l);
    } else {
        telephony_force_recover(atoi(argv[1]));
    }
    return 0;
}

/* ---------- wifi ---------- */

/* esp_console_split_argv() (esp-idf's console component) only treats double
 * quotes as a grouping delimiter -- single quotes pass through as ordinary
 * characters. So `'My Network'` arrives as two already-split tokens: `'My`
 * and `Network'`. Reassemble a run of tokens starting with a stray leading
 * "'" and ending with a trailing "'" back into one string (spaces
 * reinserted, quotes stripped), so both quoting styles work from the user's
 * side. Returns the index of the next unconsumed argv slot, or -1 if a
 * leading "'" is never closed. */
static int collect_quoted_or_plain(int argc, char **argv, int start, char *out, size_t out_len)
{
    if (start >= argc) { out[0] = 0; return start; }
    if (argv[start][0] != '\'') {
        strncpy(out, argv[start], out_len - 1);
        out[out_len - 1] = 0;
        return start + 1;
    }
    size_t len = 0;
    out[0] = 0;
    for (int i = start; i < argc; i++) {
        const char *tok = argv[i];
        size_t tok_len = strlen(tok);
        bool is_first = (i == start);
        bool ends_quoted = tok_len > 0 && tok[tok_len - 1] == '\'' && !(is_first && tok_len == 1);
        const char *seg = tok;
        size_t seg_len = tok_len;
        if (is_first) { seg++; seg_len--; }   /* strip leading ' */
        if (ends_quoted) { seg_len--; }        /* strip trailing ' */

        if (len && len + 1 < out_len) out[len++] = ' ';
        size_t copy = (seg_len < out_len - len - 1) ? seg_len : out_len - len - 1;
        memcpy(out + len, seg, copy);
        len += copy;
        out[len] = 0;

        if (ends_quoted) return i + 1;
    }
    return -1;
}

static int cmd_wifi_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: wifi_set <ssid> <password>\n"
               "Values with spaces can be quoted either way, e.g.\n"
               "  wifi_set \"My Network\" \"my password\"\n"
               "  wifi_set 'My Network' 'my password'\n");
        return 1;
    }

    static char ssid[64], pass[64];
    int next = collect_quoted_or_plain(argc, argv, 1, ssid, sizeof(ssid));
    if (next < 0) { printf("wifi_set: unterminated ' in SSID\n"); return 1; }
    next = collect_quoted_or_plain(argc, argv, next, pass, sizeof(pass));
    if (next < 0) { printf("wifi_set: unterminated ' in password\n"); return 1; }

    esp_err_t err = wifi_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        printf("wifi_set: failed to save/apply: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* Saved to NVS already (survives reboot from here on) -- no need to
     * reboot for it to take effect, but esp_wifi_connect() is async, so
     * actually wait and check rather than just reporting the synchronous
     * call succeeded. WPA3-SAE handshakes in particular can take a while
     * (observed 60s+ on real hardware), hence the generous window. */
    printf("saved to NVS, connecting to '%s' ", ssid);
    fflush(stdout);
    for (int i = 0; i < 80 && !wifi_is_connected(); i++) {
        printf(".");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("\n");
    if (wifi_is_connected()) {
        char ip[16];
        wifi_get_ip_str(ip, sizeof(ip));
        printf("connected! IP=%s -- try http://%s/ or http://%s.local/ from a browser "
               "on the same network.\n", ip, ip, CONFIG_MDNS_HOSTNAME);
        return 0;
    }
    printf("not connected after 40s. Credentials are saved, so it'll keep retrying in the "
           "background -- check 'wifi_status' in a bit, or verify the SSID/password and run "
           "wifi_set again.\n");
    return 1;
}

static int cmd_wifi_status(int argc, char **argv)
{
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    printf("wifi: %s  ssid=%s  ip=%s  rssi=%d dBm\n",
           wifi_is_connected() ? "connected" : "disconnected",
           wifi_get_ssid(), ip, wifi_rssi());
    return 0;
}

/* Compact help: one line per command ("cmd  description"), instead of the
 * built-in esp_console help command's 2-line-plus-blank-line-per-command
 * format. We already have every (command, help) pair at registration time,
 * so just stash them here rather than pulling them back out of esp_console
 * (commands.c's registry is static/internal, not exposed for enumeration). */
#define MAX_HELP_ENTRIES 32
static struct { const char *cmd; const char *help; } s_help[MAX_HELP_ENTRIES];
static int s_help_count = 0;

static void note_help(const char *cmd, const char *help)
{
    if (s_help_count < MAX_HELP_ENTRIES) {
        s_help[s_help_count].cmd = cmd;
        s_help[s_help_count].help = help;
        s_help_count++;
    }
}

static void register_cmd(const char *cmd, const char *help, esp_console_cmd_func_t fn)
{
    esp_console_cmd_t c = { .command = cmd, .help = help, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
    note_help(cmd, help);
}

static int cmd_help(int argc, char **argv)
{
    for (int i = 0; i < s_help_count; i++) {
        printf("%-13s %s\n", s_help[i].cmd, s_help[i].help);
    }
    return 0;
}

void bringup_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "phone>";
    repl_cfg.max_cmdline_length = 128;

    /* esp_console_new_repl_*() below calls esp_console_common_init()
     * internally, which unconditionally registers ESP-IDF's own "help"
     * command (the verbose 2-line-per-command one). It has to run first so
     * our compact "help" registered below -- same command name -- is the
     * one that wins and actually gets used. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* SMD boards only expose native USB (USB-Serial/JTAG) to the host --
     * UART0 (GPIO16/17) has no bridge chip and isn't wired to anything the
     * host can talk to, so the REPL has to read stdin from the same USB
     * peripheral idf.py monitor is actually connected to. Gated on the
     * primary console channel (not CONFIG_BOARD_TYPE_SMD2) because that's
     * what actually controls whether this API is even declared -- see
     * "Console output" in Component config -> ESP System Settings. */
    esp_console_dev_usb_serial_jtag_config_t usb_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_cfg, &repl_cfg, &repl));
#else
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));
#endif

    register_cmd("help", "This list", cmd_help);

    register_cmd("board_info", "Print detected board type and pin map", cmd_board_info);
    register_cmd("i2c_scan", "Scan I2C bus for the codec", cmd_i2c_scan);
    register_cmd("codec_detect", "Probe both known codec addresses, warn on board-type mismatch", cmd_codec_detect);
    register_cmd("codec_init", "Initialize the codec (ES8388 on SMD, WM8731 on THT)", cmd_codec_init);
    register_cmd("codec_dump", "Dump codec shadow registers", cmd_codec_dump);
    register_cmd("codec_bypass", "Analog bypass on/off (WM8731 only): codec_bypass <0|1>", cmd_codec_bypass);
    register_cmd("micamp", "ES8388 ADC PGA gain (SMD2): micamp <left 0-8> <right 0-8>, 3dB/step", cmd_micamp);

    tone_args.hz   = arg_dbl0(NULL, NULL, "<hz>", "frequency (default 1000)");
    tone_args.ms   = arg_int0(NULL, NULL, "<ms>", "duration ms (default 1000)");
    tone_args.line = arg_int0(NULL, NULL, "<line>", "line 0/1 (default 0)");
    tone_args.end  = arg_end(3);
    {
        esp_console_cmd_t c = {
            .command = "tone", .help = "Play a test tone: tone <hz> <ms> <line>",
            .func = cmd_tone, .argtable = &tone_args,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&c));
        note_help("tone", "Play a test tone: tone <hz> <ms> <line>");
    }

    register_cmd("dialtone", "Dial tone: dialtone [line]", cmd_progress);
    register_cmd("ringback", "Ringback tone: ringback [line]", cmd_progress);
    register_cmd("busy", "Busy tone: busy [line]", cmd_progress);
    register_cmd("silence", "Stop audio: silence [line]", cmd_silence);
    register_cmd("loopback", "Mic->speaker: loopback [line] [0|1]", cmd_loopback);
    register_cmd("capture", "RX level meter, both lines simultaneously: capture [ms]", cmd_capture);
    register_cmd("record", "Raw mic capture to storage, no live playback: record <ms> [line]", cmd_record);
    register_cmd("play", "Play a storage WAV out a line: play <name> [line]", cmd_play);
    register_cmd("dtmf_gen", "Emit DTMF: dtmf_gen <digits> [line]", cmd_dtmf_gen);
    register_cmd("slic", "SLIC pin: slic <line> <pd|fr|rm> <0|1>", cmd_slic);
    register_cmd("ring", "Ring a line: ring [line] [secs]", cmd_ring);
    register_cmd("hookmon", "Watch SHK: hookmon [secs]", cmd_hookmon);
    register_cmd("pulsemon", "Watch raw pulse dialing + timing: pulsemon [secs] [line]", cmd_pulsemon);
    register_cmd("tonemon", "Watch DTMF (touch-tone) digits + timing: tonemon [secs] [line]", cmd_tonemon);
    register_cmd("status", "Per-line telephony status", cmd_status);
    register_cmd("reset", "Force line(s) out of a stuck tone/reorder state: reset [line]", cmd_reset);
    register_cmd("wifi_set", "Save WiFi credentials + reconnect: wifi_set <ssid> <password>", cmd_wifi_set);
    register_cmd("wifi_status", "WiFi connection status", cmd_wifi_status);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "bring-up console ready — type 'help'");
}
