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
#include "aec.h"
#include "ag1170.h"
#include "dtmf.h"
#include "telephony.h"
#include "wifi.h"
#include "storage.h"
#include "sdcard.h"
#include "player.h"
#include "voip.h"
#include "logctl.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

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

/* ---------- spkamp (ES8388 LOUT1/ROUT1 earpiece drive level, live tuning) ----------
 * Companion to micamp: while a call is live and echoing, dial this down (or
 * micamp down) and see whether the echo weakens -- directly tests "is this
 * gain-dependent" without ending the call. Raw LOUT1VOL/ROUT1VOL register
 * value, not an abstracted step count like micamp (the two ES8388 volume
 * fields use different underlying encodings, so this mirrors es8388.c's own
 * es8388_set_dac_volume() convention directly rather than inventing a new
 * one). */
static int cmd_spkamp(int argc, char **argv)
{
#if CONFIG_BOARD_TYPE_SMD2
    if (argc < 3) {
        printf("usage: spkamp <left 0-63> <right 0-63>   (0=mute, 33=0dB, 1dB/step)\n"
               "current LOUT1VOL/ROUT1VOL = 0x%02X / 0x%02X\n",
               es8388_shadow(ES8388_REG_LOUT1VOL), es8388_shadow(ES8388_REG_ROUT1VOL));
        return 1;
    }
    int l = atoi(argv[1]), r = atoi(argv[2]);
    if (l < 0 || l > 63 || r < 0 || r > 63) {
        printf("left/right must each be 0-63\n");
        return 1;
    }
    esp_err_t err = es8388_set_dac_volume((uint8_t)l, (uint8_t)r);
    if (err != ESP_OK) {
        printf("write failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("LOUT1VOL=%d ROUT1VOL=%d (0dB at 33, 1dB/step)\n", l, r);
    return 0;
#else
    printf("spkamp: SMD2/ES8388 only\n");
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

/* ---------- echopulse: periodic tone burst for echo/hybrid path debugging ----------
 * Plays a short, distinctive 1kHz burst on `line` every `period_ms`, logging
 * the exact esp_timer_get_time() of each one to correlate against an
 * oscilloscope trace or your own ears/recording -- gives a precisely-timed,
 * known signal instead of trying to eyeball delay in a live conversation.
 * Goes out through the same DAC -> codec -> SLIC path a real call uses (NOT
 * the digital test_loopback tap, which explicitly bypasses the analog
 * codec/hybrid you're trying to characterize), so it exercises the actual
 * hardware. Runs independent of hook/call state -- it's a raw audio-layer
 * primitive -- so you can test purely locally (probe the codec output and,
 * if you have a second point, the mic/ADC input, and see whether the pulse
 * reappears there and how long it takes) or during a real call to the other
 * board/Linphone (have the far end listen for the periodic pulse arriving,
 * then listen on *this* end for a delayed, quieter repeat of it -- that
 * repeat's timing, measured against this command's own timestamps, is your
 * echo path length, hard numbers instead of a guess). */
#define ECHOPULSE_MAX_LINES 2
/* Short on purpose: long enough to be an unmistakable 1kHz burst (10 cycles),
 * short enough to see the whole pulse -- and any echo of it -- in one
 * oscilloscope sweep without a long flat top obscuring the edges. */
#define ECHOPULSE_BURST_MS 10
static TaskHandle_t s_echopulse_task[ECHOPULSE_MAX_LINES];
static volatile int s_echopulse_period_ms[ECHOPULSE_MAX_LINES];

static void echopulse_task(void *arg)
{
    int line = (int)(intptr_t)arg;
    while (1) {
        printf("echopulse line %d: PULSE at t=%lld ms\n",
               line, (long long)(esp_timer_get_time() / 1000));
        audio_tx_burst(line, 1000.0f, 0.0f, 0.6f, ECHOPULSE_BURST_MS);
        int period = s_echopulse_period_ms[line];
        vTaskDelay(pdMS_TO_TICKS((period > ECHOPULSE_BURST_MS) ? (period - ECHOPULSE_BURST_MS) : period));
    }
}

static int cmd_echopulse(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: echopulse <line> <on|off> [period_ms, default 2000]\n");
        return 1;
    }
    int line = atoi(argv[1]);
    if (line < 0 || line >= ECHOPULSE_MAX_LINES) {
        printf("bad line (0-%d)\n", ECHOPULSE_MAX_LINES - 1);
        return 1;
    }
    if (strcmp(argv[2], "on") == 0) {
        if (s_echopulse_task[line]) {
            printf("echopulse already running on line %d\n", line);
            return 0;
        }
        s_echopulse_period_ms[line] = (argc > 3) ? atoi(argv[3]) : 2000;
        xTaskCreate(echopulse_task, "echopulse", 3072, (void *)(intptr_t)line,
                    tskIDLE_PRIORITY + 2, &s_echopulse_task[line]);
        printf("echopulse line %d: started, 1kHz/%dms burst every %d ms\n",
               line, ECHOPULSE_BURST_MS, s_echopulse_period_ms[line]);
    } else {
        if (s_echopulse_task[line]) {
            vTaskDelete(s_echopulse_task[line]);
            s_echopulse_task[line] = NULL;
        }
        printf("echopulse line %d: stopped\n", line);
    }
    return 0;
}

/* ---------- echosweep: step through several frequencies for echo debugging ----------
 * echopulse's fixed 1kHz burst didn't reliably reproduce the reported echo,
 * where real speech did -- one real possibility is a handset with its own
 * mic->earpiece acoustic feed-through (separate from anything on this
 * board) that couples some frequencies much better than others, the way
 * any small acoustic cavity (earpiece/mouthpiece) tends to have real
 * resonances. This cycles through 9 tones spanning classic telephone
 * bandwidth (300-3400Hz) so you can listen for -- or capture and compare --
 * whether some frequencies echo more than others, rather than assuming a
 * single 1kHz probe represents the whole band. Same real DAC->codec->SLIC
 * path as echopulse, same reasoning for why (exercises actual hardware,
 * not the digital test_loopback tap). */
#define ECHOSWEEP_MAX_LINES 2
#define ECHOSWEEP_BURST_MS 150 /* longer than echopulse's 10ms -- audible/identifiable by ear */
static const float ECHOSWEEP_FREQS_HZ[] = { 300, 500, 700, 1000, 1500, 2000, 2500, 3000, 3400 };
#define ECHOSWEEP_N_FREQS (sizeof(ECHOSWEEP_FREQS_HZ) / sizeof(ECHOSWEEP_FREQS_HZ[0]))
static TaskHandle_t s_echosweep_task[ECHOSWEEP_MAX_LINES];
static volatile int s_echosweep_period_ms[ECHOSWEEP_MAX_LINES];

static void echosweep_task(void *arg)
{
    int line = (int)(intptr_t)arg;
    size_t i = 0;
    while (1) {
        float hz = ECHOSWEEP_FREQS_HZ[i % ECHOSWEEP_N_FREQS];
        printf("echosweep line %d: %.0f Hz PULSE at t=%lld ms\n",
               line, hz, (long long)(esp_timer_get_time() / 1000));
        audio_tx_burst(line, hz, 0.0f, 0.6f, ECHOSWEEP_BURST_MS);
        i++;
        int period = s_echosweep_period_ms[line];
        vTaskDelay(pdMS_TO_TICKS((period > ECHOSWEEP_BURST_MS) ? (period - ECHOSWEEP_BURST_MS) : period));
    }
}

static int cmd_echosweep(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: echosweep <line> <on|off> [period_ms, default 2000]\n");
        printf("cycles through: ");
        for (size_t i = 0; i < ECHOSWEEP_N_FREQS; i++) printf("%.0fHz ", ECHOSWEEP_FREQS_HZ[i]);
        printf("\n");
        return 1;
    }
    int line = atoi(argv[1]);
    if (line < 0 || line >= ECHOSWEEP_MAX_LINES) {
        printf("bad line (0-%d)\n", ECHOSWEEP_MAX_LINES - 1);
        return 1;
    }
    if (strcmp(argv[2], "on") == 0) {
        if (s_echosweep_task[line]) {
            printf("echosweep already running on line %d\n", line);
            return 0;
        }
        s_echosweep_period_ms[line] = (argc > 3) ? atoi(argv[3]) : 2000;
        xTaskCreate(echosweep_task, "echosweep", 3072, (void *)(intptr_t)line,
                    tskIDLE_PRIORITY + 2, &s_echosweep_task[line]);
        printf("echosweep line %d: started, %dms bursts every %d ms, cycling %u frequencies\n",
               line, ECHOSWEEP_BURST_MS, s_echosweep_period_ms[line], (unsigned)ECHOSWEEP_N_FREQS);
    } else {
        if (s_echosweep_task[line]) {
            vTaskDelete(s_echosweep_task[line]);
            s_echosweep_task[line] = NULL;
        }
        printf("echosweep line %d: stopped\n", line);
    }
    return 0;
}

/* ---------- call-progress tones ---------- */
static int cmd_progress(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    call_progress_t t = TONE_DIAL;
    if (strcmp(argv[0], "ringback") == 0) t = TONE_RINGBACK;
    else if (strcmp(argv[0], "busy") == 0) t = TONE_BUSY;
    else if (strcmp(argv[0], "roh") == 0) t = TONE_OFFHOOK_ALERT;
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
/* record's tap callback: runs on the audio task (audio.c's audio_task()),
 * same synchronous point DTMF detection taps in from -- must stay fast, no
 * blocking/malloc/logging, so it just hands samples off to a dedicated
 * StreamBufferHandle_t (created fresh per recording, single producer here /
 * single consumer in cmd_record() below -- NOT the shared per-line rx stream
 * buffer the RTP task owns, so this can't hit the same-buffer, two-readers
 * crash audio_rx_read()'s doc comment describes). Non-blocking send: if the
 * console task somehow falls behind and the ring fills, samples are dropped
 * rather than blocking the audio task. */
typedef struct {
    StreamBufferHandle_t sb;
} record_tap_ctx_t;

static void record_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line;
    record_tap_ctx_t *rc = ctx;
    xStreamBufferSend(rc->sb, mono, n * sizeof(int16_t), 0);
}

/* Streams straight to storage (storage_write_begin/chunk/end + storage_patch
 * for the header sizes once the final length's known -- same pattern
 * recorder.c uses) instead of one big malloc for the whole clip. That's what
 * makes a real 10-15s capture possible *during* an active call: RAM is tight
 * enough mid-call that the old single-malloc version got capped to ~1s,
 * nowhere near enough to coordinate a manual echo test. */
#define RECORD_CHUNK_SAMPLES 1536
#define RECORD_SB_BYTES (RECORD_CHUNK_SAMPLES * 8 * sizeof(int16_t))

/* Same format/fallback as telephony.c's rec_name_timestamped() (the "record
 * message" feature) -- duplicated rather than shared since it's a tiny,
 * self-contained helper (matches this file's own wav_put_u16/u32 style of
 * not pulling in a cross-component utility for a few lines). Wall-clock date
 * + time (UTC) once SNTP has synced, so two console recordings don't collide
 * under a bare uptime-ms name and are actually meaningful to sort/find later;
 * falls back to a boot-relative tag before sync (guards against every early
 * recording naming itself "19700101_000003.wav" and colliding with the
 * next one). */
static void record_name_timestamped(char *out, size_t out_len, int line)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        snprintf(out, out_len, "rec_l%d_boot%lld.wav", line,
                 (long long)(esp_timer_get_time() / 1000));
        return;
    }
    struct tm t;
    gmtime_r(&now, &t);
    snprintf(out, out_len, "rec_l%d_%04d%02d%02d_%02d%02d%02dZ.wav", line,
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

/* ---------- echoprobe: measure the LOCAL earpiece -> mic echo path ----------
 *
 * The ~355 ms figure measured end-to-end over a call is ROUND-TRIP NETWORK
 * delay (mic -> RTP -> Asterisk -> RTP -> far earpiece -> leak -> back
 * again). That governs how *annoying* the echo is (ITU-T G.131), but it is
 * NOT what sizes an echo canceller: an AEC runs on the leaking device and
 * models only its own earpiece -> mic path. What that filter has to span is
 * the LOCAL bulk delay -- I2S TX DMA depth + codec group delay + the
 * acoustic/hybrid hop + I2S RX DMA depth -- plus however long the impulse
 * response rings.
 *
 * So: emit a short burst, capture the mic through the diagnostic tap (which
 * is safe to run alongside anything else, unlike audio_rx_read), and report
 * where the burst comes back and how strongly. Run it with the handset
 * off-hook and NOT in a call, so the only thing the mic can hear is this
 * board's own earpiece.
 *
 * Two numbers come out of it, both needed before writing a canceller:
 *   - bulk delay (ms)  -> how far back the reference has to be aligned
 *   - tail length (ms) -> how many adaptive taps are actually required
 */
#define ECHOPROBE_MS        400   /* capture window */
#define ECHOPROBE_BURST_MS  10
#define ECHOPROBE_BURST_HZ  1000.0f

static int cmd_echoprobe(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    if (line < 0 || line > 1) { printf("usage: echoprobe [line]\n"); return 1; }

    uint32_t sr = audio_sample_rate();
    size_t n_samples = (size_t)((uint64_t)sr * ECHOPROBE_MS / 1000);

    int16_t *cap = malloc(n_samples * sizeof(int16_t));
    if (!cap) { printf("malloc failed\n"); return 1; }

    StreamBufferHandle_t sb = xStreamBufferCreate(RECORD_SB_BYTES, 1);
    if (!sb) { free(cap); printf("xStreamBufferCreate failed\n"); return 1; }

    printf("echoprobe line %d: %.0f Hz burst %d ms, listening %d ms...\n",
           line, ECHOPROBE_BURST_HZ, ECHOPROBE_BURST_MS, ECHOPROBE_MS);

    record_tap_ctx_t rc = { .sb = sb };
    audio_add_diag_tap(line, record_tap, &rc);
    /* Tap first, THEN emit: starting the burst before the tap is armed would
     * lose the very leading edge this measurement is all about. */
    audio_tx_burst(line, ECHOPROBE_BURST_HZ, 0.0f, 0.8f, ECHOPROBE_BURST_MS);

    size_t got = 0;
    while (got < n_samples) {
        size_t want = n_samples - got;
        if (want > RECORD_CHUNK_SAMPLES) want = RECORD_CHUNK_SAMPLES;
        size_t n = xStreamBufferReceive(sb, cap + got, want * sizeof(int16_t),
                                        pdMS_TO_TICKS(500));
        if (n == 0) break;
        got += n / sizeof(int16_t);
    }
    audio_remove_diag_tap(line);
    vStreamBufferDelete(sb);
    audio_tx_silence(line);

    if (got == 0) { free(cap); printf("no audio captured\n"); return 1; }

    /* Peak + noise floor. The floor is taken from the first 2 ms, which is
     * always before any plausible echo return (bulk delay is dominated by
     * DMA buffering, tens of ms) -- so it measures ambient/self-noise, not
     * the thing we're looking for. */
    size_t floor_n = (size_t)(sr * 2 / 1000);
    if (floor_n > got) floor_n = got;
    uint64_t acc = 0;
    for (size_t i = 0; i < floor_n; i++) acc += (uint64_t)((int32_t)cap[i] * cap[i]);
    uint32_t noise_rms = floor_n ? (uint32_t)sqrt((double)acc / (double)floor_n) : 0;

    int32_t peak = 0; size_t peak_i = 0;
    for (size_t i = 0; i < got; i++) {
        int32_t a = cap[i] < 0 ? -cap[i] : cap[i];
        if (a > peak) { peak = a; peak_i = i; }
    }

    /* Tail: last sample still above max(4x noise floor, 5% of peak), scanned
     * from the end so a mid-burst dip doesn't cut it short. */
    int32_t thresh = (int32_t)noise_rms * 4;
    if (thresh < peak / 20) thresh = peak / 20;
    size_t tail_i = peak_i;
    for (size_t i = got; i > peak_i; i--) {
        int32_t a = cap[i - 1] < 0 ? -cap[i - 1] : cap[i - 1];
        if (a > thresh) { tail_i = i - 1; break; }
    }

    printf("  captured %u ms, noise floor rms=%u\n",
           (unsigned)((uint64_t)got * 1000 / sr), (unsigned)noise_rms);
    printf("  peak %d at %.1f ms  <- bulk delay (DMA + codec + acoustic)\n",
           (int)peak, (double)peak_i * 1000.0 / (double)sr);
    printf("  decays below %d by %.1f ms  -> tail ~%.1f ms after peak\n",
           (int)thresh, (double)tail_i * 1000.0 / (double)sr,
           (double)(tail_i - peak_i) * 1000.0 / (double)sr);
    if (peak < (int32_t)noise_rms * 4) {
        printf("  NOTE: peak barely above noise -- little/no local leak on this line,\n"
               "        or the handset is on-hook. AEC would have nothing to cancel.\n");
    } else {
        printf("  => AEC filter must span ~%.0f ms (%u taps @ 8 kHz)\n",
               (double)tail_i * 1000.0 / (double)sr,
               (unsigned)((uint64_t)tail_i * 8000 / sr));
    }
    free(cap);
    return 0;
}

/* ---------- aec (echo canceller control + live measurement) ---------- */
static int cmd_aec_sweep(int argc, char **argv);

static int cmd_aec(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: aec <on|off|status|reset|sweep|delay <ms>> [line]\n");
        return 1;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "sweep") == 0) return cmd_aec_sweep(argc - 1, &argv[1]);

    int line = 0;
    if (strcmp(sub, "delay") == 0) { if (argc > 3) line = atoi(argv[3]); }
    else if (argc > 2) line = atoi(argv[2]);
    if (line < 0 || line > 1) { printf("line must be 0 or 1\n"); return 1; }

    if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        aec_set_enabled(line, strcmp(sub, "on") == 0);
        printf("line %d: AEC %s\n", line, aec_is_enabled(line) ? "on" : "off");
        return 0;
    }
    if (strcmp(sub, "reset") == 0) {
        aec_reset(line);
        printf("line %d: AEC filter reset\n", line);
        return 0;
    }
    if (strcmp(sub, "delay") == 0) {
        if (argc < 3) { printf("usage: aec delay <ms> [line]\n"); return 1; }
        uint32_t ms = (uint32_t)atoi(argv[2]);
        if (aec_set_delay_ms(line, ms) != ESP_OK) {
            printf("delay must be 0-%d ms\n", AEC_MAX_DELAY_MS);
            return 1;
        }
        printf("line %d: AEC bulk delay %u ms (filter reset)\n", line, (unsigned)ms);
        return 0;
    }
    if (strcmp(sub, "status") == 0) {
        aec_status_t st;
        aec_get_status(line, &st);
        printf("line %d AEC: %s, delay %u ms, %d taps\n", line,
               st.enabled ? "ON" : "off", (unsigned)st.delay_ms, AEC_TAPS);
        printf("  ERLE %d dB   adaptation frozen %d%% of blocks   %s\n",
               st.erle_db, st.freeze_pct,
               st.converged ? "converged" : "not converged yet");
        printf("  (ERLE is how much quieter the outgoing signal got. It only\n"
               "   means anything while the FAR end is talking -- there is no\n"
               "   echo to cancel during silence. Stats reset on each read.)\n");
        return 0;
    }
    printf("unknown subcommand '%s'\n", sub);
    return 1;
}

/* ---------- aec sweep (auto-search the bulk delay) ----------
 *
 * `echoprobe`'s bulk-delay measurement turned out NOT to be directly usable
 * as `aec delay`: it timestamps end-to-end (console command -> burst ->
 * captured echo), while the canceller pairs `ref`/`near` samples that are
 * already sitting together within one audio_task iteration -- a different
 * reference point with its own inherent frame-level offset echoprobe never
 * saw. Confirmed live: echoprobe measured MaxPhone's bulk delay at 36 ms,
 * but `aec delay 36` measured WORSE (0-3 dB ERLE) than the default 28 ms
 * (10 dB). So: search empirically against the canceller's own ERLE instead
 * of trusting the hardware measurement -- also means this adapts to a
 * different board/handset without needing new firmware, just a re-run.
 *
 * Needs real, continuous far-end audio for the whole sweep -- ERLE is
 * meaningless against silence (nothing to cancel). Each candidate gets a
 * settle period (let the filter reconverge from the reset aec_set_delay_ms()
 * causes) before its dwell period (accumulate stats) is read.
 */
#define AEC_SWEEP_SETTLE_MS  200
#define AEC_SWEEP_DWELL_MS   400
#define AEC_SWEEP_COARSE_STEP_MS 4

static int aec_sweep_try(int line, uint32_t delay_ms)
{
    aec_set_delay_ms(line, delay_ms); /* resets the filter */
    vTaskDelay(pdMS_TO_TICKS(AEC_SWEEP_SETTLE_MS));
    aec_status_t discard;
    aec_get_status(line, &discard); /* clear settle-period stats */
    vTaskDelay(pdMS_TO_TICKS(AEC_SWEEP_DWELL_MS));
    aec_status_t st;
    aec_get_status(line, &st);
    printf("  %2u ms: ERLE %3d dB%s\n", (unsigned)delay_ms, st.erle_db,
           st.freeze_pct > 80 ? "  (mostly frozen -- near end talking too?)" : "");
    return st.erle_db;
}

static int cmd_aec_sweep(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    if (line < 0 || line > 1) { printf("usage: aec sweep [line]\n"); return 1; }

    if (telephony_state(line) != TEL_INCALL) {
        printf("line %d is not in a call (state=%s) -- sweep needs live far-end "
               "audio to measure against. Get a call connected first.\n",
               line, telephony_state_name(telephony_state(line)));
        return 1;
    }

    uint32_t total_steps = (AEC_MAX_DELAY_MS / AEC_SWEEP_COARSE_STEP_MS) + 1 + 7;
    printf("Sweeping AEC delay on line %d -- keep the FAR END talking continuously "
           "for the next ~%u s.\n", line,
           (unsigned)(total_steps * (AEC_SWEEP_SETTLE_MS + AEC_SWEEP_DWELL_MS) / 1000));
    aec_set_enabled(line, true);

    printf("coarse pass (%u ms steps):\n", AEC_SWEEP_COARSE_STEP_MS);
    int best_delay = 0, best_erle = -1000;
    for (uint32_t d = 0; d <= AEC_MAX_DELAY_MS; d += AEC_SWEEP_COARSE_STEP_MS) {
        int erle = aec_sweep_try(line, d);
        if (erle > best_erle) { best_erle = erle; best_delay = (int)d; }
    }

    int fine_lo = best_delay - 3; if (fine_lo < 0) fine_lo = 0;
    int fine_hi = best_delay + 3; if (fine_hi > AEC_MAX_DELAY_MS) fine_hi = AEC_MAX_DELAY_MS;
    printf("fine pass around %d ms:\n", best_delay);
    for (int d = fine_lo; d <= fine_hi; d++) {
        if (d % AEC_SWEEP_COARSE_STEP_MS == 0) continue; /* already measured in the coarse pass */
        int erle = aec_sweep_try(line, d);
        if (erle > best_erle) { best_erle = erle; best_delay = d; }
    }

    aec_set_delay_ms(line, (uint32_t)best_delay);
    aec_reset(line);
    if (best_erle < 3) {
        printf("Best found: %d ms, but ERLE never exceeded %d dB anywhere in the "
               "sweep -- was the far end actually talking the whole time? Applied "
               "anyway, but this doesn't look like a confident result. Try again.\n",
               best_delay, best_erle);
    } else {
        printf("Best: %d ms (ERLE %d dB). Delay set, filter reset, AEC left on.\n",
               best_delay, best_erle);
    }
    return 0;
}

static int cmd_record(int argc, char **argv)
{
    if (argc < 2) { printf("usage: record <ms> [line]\n"); return 1; }
    int ms = atoi(argv[1]);
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    if (ms <= 0 || line < 0 || line > 1) { printf("bad args\n"); return 1; }

    uint32_t sr = audio_sample_rate();
    size_t n_samples = (size_t)((int64_t)sr * ms / 1000);
    size_t expected_bytes = 44 + n_samples * sizeof(int16_t);

    char name[STORAGE_MAX_NAME];
    record_name_timestamped(name, sizeof(name), line);

    storage_writer_t w = {0};
    if (storage_write_begin(name, expected_bytes, &w) != ESP_OK) {
        printf("storage_write_begin failed (not enough space for %u bytes?)\n",
               (unsigned)expected_bytes);
        return 1;
    }

    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    wav_put_u32(hdr + 4, 36); /* sizes patched at the end */
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wav_put_u32(hdr + 16, 16);
    wav_put_u16(hdr + 20, 1);            /* PCM */
    wav_put_u16(hdr + 22, 1);            /* mono */
    wav_put_u32(hdr + 24, sr);
    wav_put_u32(hdr + 28, sr * 2);       /* byte rate: sr * channels * bytes/sample */
    wav_put_u16(hdr + 32, 2);            /* block align */
    wav_put_u16(hdr + 34, 16);           /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    wav_put_u32(hdr + 40, 0);
    if (storage_write_chunk(&w, hdr, sizeof(hdr)) != ESP_OK) {
        storage_write_abort(&w);
        printf("storage_write_chunk (header) failed\n");
        return 1;
    }

    printf("recording line %d for %u ms (%u samples)...\n",
           line, (unsigned)((uint64_t)n_samples * 1000 / sr), (unsigned)n_samples);

    StreamBufferHandle_t sb = xStreamBufferCreate(RECORD_SB_BYTES, 1);
    if (!sb) {
        storage_write_abort(&w);
        printf("xStreamBufferCreate failed\n");
        return 1;
    }
    record_tap_ctx_t rc = { .sb = sb };
    audio_add_diag_tap(line, record_tap, &rc);

    /* Heap, not a stack array: this runs directly on the console REPL task,
     * whose stack is sized for ordinary command handlers, not a few KB of
     * local buffer on top of printf's/argtable's own usage -- a stack
     * version of this crashed with a stack-protection fault. recorder.c's
     * task-based capture can afford a stack array because it runs on its
     * own dedicated 4096-byte task; this can't assume that. */
    int16_t *chunk = malloc(RECORD_CHUNK_SAMPLES * sizeof(int16_t));
    size_t got = 0;
    esp_err_t werr = ESP_OK;
    if (!chunk) {
        printf("malloc failed\n");
        werr = ESP_ERR_NO_MEM;
    }
    /* Generous per-chunk timeout: capture should never actually starve this
     * long (the audio task feeds it every ~10ms), so hitting it means the
     * audio task itself has stalled -- stop rather than hang the console. */
    while (chunk && got < n_samples && werr == ESP_OK) {
        size_t want = n_samples - got;
        if (want > RECORD_CHUNK_SAMPLES) want = RECORD_CHUNK_SAMPLES;
        size_t n = xStreamBufferReceive(sb, chunk, want * sizeof(int16_t), pdMS_TO_TICKS(2000));
        if (n == 0) {
            printf("capture stalled after %u/%u samples\n", (unsigned)got, (unsigned)n_samples);
            break;
        }
        size_t nsamp = n / sizeof(int16_t);
        werr = storage_write_chunk(&w, chunk, nsamp * sizeof(int16_t));
        got += nsamp;
    }
    free(chunk);

    audio_remove_diag_tap(line);
    vStreamBufferDelete(sb);

    if (werr != ESP_OK) {
        storage_write_abort(&w);
        printf("storage_write_chunk failed: %s\n", esp_err_to_name(werr));
        return 1;
    }
    storage_write_end(&w);

    size_t data_bytes = got * sizeof(int16_t);
    uint8_t sz[4];
    wav_put_u32(sz, 36 + (uint32_t)data_bytes); storage_patch(name, 4, sz, 4);
    wav_put_u32(sz, (uint32_t)data_bytes);      storage_patch(name, 40, sz, 4);

    size_t final_bytes = 44 + data_bytes;
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
    printf("Reads the raw pulse counter directly (doesn't touch the AG1170_EV_DIGIT callback\n"
           "telephony uses), and suppresses the dial plan for the duration so what you dial\n"
           "here doesn't also register as a real call attempt (wrong number, hangup, etc).\n"
           "Hook state and ringing are unaffected.\n");

    telephony_set_monitor_suppress(true);
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
    telephony_set_monitor_suppress(false);
    return 0;
}

/* ---------- tonemon ---------- */
static int cmd_tonemon(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 15;
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    printf("Monitoring line %d for DTMF (touch-tone) digits for %d s (dial digits now, or "
           "run 'dtmf_gen <digits> %d' from another session)...\n", line, secs, line);
    printf("Reads telephony's own always-running Goertzel detector directly (doesn't touch\n"
           "the RX tap), and suppresses the dial plan for the duration so what you dial here\n"
           "doesn't also register as a real call attempt (wrong number, hangup, etc). Hook\n"
           "state and ringing are unaffected.\n");

    telephony_set_monitor_suppress(true);
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
    telephony_set_monitor_suppress(false);
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

/* ---------- logging ---------- */
static int cmd_log_level(int argc, char **argv)
{
    if (argc < 2) {
        printf("log level: %s\n", logctl_level_name(logctl_get_level()));
        printf("usage: log_level <none|error|warn|info|debug|verbose>\n");
        printf("  debug shows rotary pulse + hook-switch timing and FSM state changes\n");
        return 0;
    }
    esp_log_level_t lvl = logctl_level_from_name(argv[1]);
    if (lvl > ESP_LOG_VERBOSE) {
        printf("bad level '%s' (use none|error|warn|info|debug|verbose)\n", argv[1]);
        return 1;
    }
    logctl_set_level(lvl);
    printf("log level set to %s (persisted)\n", logctl_level_name(lvl));
    return 0;
}

/* ---------- test / DVT harness ---------- */
static int cmd_testmode(int argc, char **argv)
{
    if (argc < 2) {
        printf("test mode: %s\n", telephony_test_enabled() ? "ON" : "off");
        printf("usage: testmode <0|1>  (gates tinject hook/digit injection)\n");
        return 0;
    }
    telephony_test_set_enabled(argv[1][0] == '1');
    printf("test mode %s\n", telephony_test_enabled() ? "ON" : "off");
    return 0;
}

static int cmd_tinject(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: tinject <offhook|onhook|digit|dial> [line] [digit(s)]\n"
               "  tinject offhook 0        simulate handset pickup on line 0\n"
               "  tinject dial 0 402       pick digits (feeds 4,0,2 with spacing)\n"
               "  tinject onhook 0         simulate hang-up\n"
               "requires test mode: run 'testmode 1' first\n");
        return 1;
    }
    if (!telephony_test_enabled()) { printf("enable test mode first: testmode 1\n"); return 1; }
    int line = (argc >= 3 && argv[2][0] >= '0' && argv[2][0] <= '9') ? atoi(argv[2]) : 0;
    esp_err_t err = ESP_OK;
    if (strcmp(argv[1], "offhook") == 0)      err = telephony_test_hook(line, true);
    else if (strcmp(argv[1], "onhook") == 0)  err = telephony_test_hook(line, false);
    else if (strcmp(argv[1], "digit") == 0) {
        if (argc < 4) { printf("usage: tinject digit <line> <d>\n"); return 1; }
        err = telephony_test_digit(line, argv[3][0]);
    } else if (strcmp(argv[1], "dial") == 0) {
        if (argc < 4) { printf("usage: tinject dial <line> <digits>\n"); return 1; }
        for (const char *d = argv[3]; *d && err == ESP_OK; d++) {
            err = telephony_test_digit(line, *d);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    } else { printf("unknown: %s\n", argv[1]); return 1; }
    if (err != ESP_OK) { printf("inject failed: %s\n", esp_err_to_name(err)); return 1; }
    printf("injected (read 'status' to see result)\n");
    return 0;
}

static int cmd_txrx_loopback(int argc, char **argv)
{
    if (argc < 2) {
        printf("txrx loopback: %s\n", audio_test_loopback() ? "ON" : "off");
        printf("usage: txrx_loopback <0|1>  (feed synthesized TX back into RX detect/level)\n");
        return 0;
    }
    audio_set_test_loopback(argv[1][0] == '1');
    printf("txrx loopback %s\n", audio_test_loopback() ? "ON" : "off");
    return 0;
}

/* ---------- VoIP (SIP) ---------- */
static int cmd_sip_set(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: sip_set <server> <user0> <pass0> [user1] [pass1]\n"
               "  e.g. sip_set feynman.local phone <pass> phone2 <pass2>\n"
               "  server may be a hostname, <name>.local, or an IP.\n"
               "Saved to NVS; reboot to (re)register.\n");
        return 1;
    }
    const char *server = argv[1];
    const char *user0  = argv[2];
    const char *pass0  = argv[3];
    const char *user1  = (argc > 4) ? argv[4] : NULL;
    const char *pass1  = (argc > 5) ? argv[5] : NULL;
    esp_err_t err = voip_set_config(server, user0, pass0, user1, pass1);
    if (err != ESP_OK) { printf("sip_set: save failed: %s\n", esp_err_to_name(err)); return 1; }
    printf("saved. Reboot ('reset' won't re-init SIP; power-cycle or re-flash) to apply.\n");
    return 0;
}

static int cmd_sip_status(int argc, char **argv)
{
    printf("sip server: %s\n", voip_server());
    printf("  line 0: %s\n", voip_is_registered(0) ? "REGISTERED" : "not registered");
    printf("  line 1: %s\n", voip_is_registered(1) ? "REGISTERED" : "not registered");
    printf("(stub=%s)\n", voip_is_stub() ? "yes" : "no");
    return 0;
}

static int cmd_sip_call(int argc, char **argv)
{
    if (argc < 2) { printf("usage: sip_call <number, e.g. 5551234> [line]\n"
                           "NOTE: bypasses the handset FSM -- signaling/RTP only, no earpiece audio.\n"); return 1; }
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    esp_err_t err = voip_dial(line, argv[1]);
    printf("voip_dial(line=%d, %s) -> %s\n", line, argv[1], esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_sip_hangup(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    voip_hangup(line);
    printf("hung up line %d\n", line);
    return 0;
}

/* ---------- microSD ---------- */
static int cmd_sd_test(int argc, char **argv)
{
    if (!sdcard_mounted()) {
        printf("SD not mounted; trying to mount now...\n");
        esp_err_t err = sdcard_init();
        if (err != ESP_OK) {
            printf("mount failed: %s\n", esp_err_to_name(err));
            return 1;
        }
    }
    sdcard_print_info();
    return sdcard_selftest() == ESP_OK ? 0 : 1;
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
    /* Board name (lowercased), not the generic "phone>" -- static, since the
     * REPL just holds a pointer to this for its whole lifetime. Once useful
     * when you're juggling two boards' monitor sessions side by side (which
     * log is which stops being a guess). */
    static char s_prompt[40];
    char name[32];
    board_name_get(name, sizeof(name));
    size_t i = 0;
    for (; name[i] && i < sizeof(s_prompt) - 2; i++) {
        s_prompt[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];
    }
    s_prompt[i++] = '>';
    s_prompt[i] = '\0';
    repl_cfg.prompt = s_prompt;
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
    register_cmd("spkamp", "ES8388 earpiece drive level (SMD2): spkamp <left 0-63> <right 0-63>, 1dB/step, 33=0dB", cmd_spkamp);

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
    register_cmd("roh", "Off-hook (receiver-off-hook) alert tone: roh [line]", cmd_progress);
    register_cmd("silence", "Stop audio: silence [line]", cmd_silence);
    register_cmd("loopback", "Mic->speaker: loopback [line] [0|1]", cmd_loopback);
    register_cmd("capture", "RX level meter, both lines simultaneously: capture [ms]", cmd_capture);
    register_cmd("echopulse", "Periodic 1kHz burst for echo/hybrid debugging: echopulse <line> <on|off> [period_ms]", cmd_echopulse);
    register_cmd("echosweep", "Cycles 9 tones 300-3400Hz for echo/resonance debugging: echosweep <line> <on|off> [period_ms]", cmd_echosweep);
    register_cmd("record", "Raw mic capture to storage, no live playback, safe during an active call: record <ms> [line]", cmd_record);
    register_cmd("aec", "Echo canceller: aec <on|off|status|reset|sweep|delay <ms>> [line] "
                        "(sweep needs a live call with the far end talking)", cmd_aec);
    register_cmd("echoprobe", "Measure the local earpiece->mic echo path (bulk delay + tail) for AEC sizing: echoprobe [line]", cmd_echoprobe);
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
    register_cmd("log_level", "Get/set global log level: log_level [none|error|warn|info|debug|verbose]", cmd_log_level);
    register_cmd("testmode", "Enable/disable telephony injection test mode: testmode <0|1>", cmd_testmode);
    register_cmd("tinject", "Inject handset events (test mode): tinject <offhook|onhook|digit|dial> [line] [d]", cmd_tinject);
    register_cmd("txrx_loopback", "Audio TX->RX digital loopback for tests: txrx_loopback <0|1>", cmd_txrx_loopback);
    register_cmd("sip_set", "Save SIP config: sip_set <server> <user0> <pass0> [user1] [pass1]", cmd_sip_set);
    register_cmd("sip_status", "SIP registration status", cmd_sip_status);
    register_cmd("sip_call", "Test SIP INVITE (signaling only): sip_call <number> [line]", cmd_sip_call);
    register_cmd("sip_hangup", "Hang up a SIP call: sip_hangup [line]", cmd_sip_hangup);
    register_cmd("sd_test", "microSD: mount (if needed), print card info, write/read/verify", cmd_sd_test);

    /* esp_console_new_repl_*() above probes the terminal with a "device
     * status request" escape sequence (ESC[5n) and waits up to 500ms for the
     * reply. Over native USB (Serial/JTAG) + idf.py monitor's extra buffering
     * layer, that reply can arrive *after* the probe gives up -- it then
     * sits in stdin and gets read as if typed, printing as garbage escape
     * bytes right after your first real command. Give it a moment, then
     * throw away anything that trickled in late before the REPL starts
     * actually reading commands. */
    vTaskDelay(pdMS_TO_TICKS(700));
    {
        int fd = fileno(stdin);
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        char junk[32];
        while (read(fd, junk, sizeof(junk)) > 0) { /* discard stale probe response */ }
        fcntl(fd, F_SETFL, flags);
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "bring-up console ready — type 'help'");
}
