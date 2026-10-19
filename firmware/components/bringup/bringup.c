#include "bringup.h"
#include "board.h"
#include "board_pins.h"
#include "board_config.h"
#include "es8388.h"
#include "audio.h"
#include "aec.h"
#include "ag1170.h"
#include "dtmf.h"
#include "telephony.h"
#include "selftest.h"
#include "idle_power.h"
#include "webauth.h"
#include "webui.h"
#include "auxpins.h"
#include "dialplan.h"
#include "esp_pm.h"
#include "wifi.h"
#include "esp_wifi_types.h"   /* WIFI_AUTH_* for wifi_scan's output */
#include "storage.h"
#include "sdcard.h"
#include "player.h"
#include "voip.h"
#include "bt_companion.h"
#include "bk3266.h"
#include "bk3266_proto.h"
#include "driver/gpio.h"
#include "logctl.h"
#include "esp_console.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "linenoise/linenoise.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "soc/lp_aon_reg.h"
#include "soc/soc.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

static const char *TAG = "bringup";

/* ---------- i2c_scan ---------- */
/* webpass [clear]: the web UI password (webauth.h) -- shows whether one is
 * set; `clear` removes it, for a forgotten password. */
static int cmd_webpass(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        webauth_clear();
        printf("Web password removed: the web UI no longer asks for one.\n");
        return 0;
    }
    printf("Web password: %s. 'webpass clear' removes it.\n", webauth_enabled() ? "set" : "not set");
    return 0;
}

/* auxpin [<gpio> <secs>]: drive one of the spare pins (18-21) high for secs
 * seconds, or toggle with 0. With no arguments: shows whether they're usable
 * and each pin's level. */
static int cmd_auxpin(int argc, char **argv)
{
    char why[160];
    bool usable = auxpins_usable(why, sizeof(why));
    int gp[AUXPINS_COUNT];
    size_t n = auxpins_list(gp, AUXPINS_COUNT);
    if (argc < 3) {
        printf("spare pins %s%s%s\n", usable ? "usable" : "NOT usable: ", usable ? "" : why, "");
        for (size_t i = 0; i < n; i++) printf("  GPIO%d  %s\n", gp[i], auxpins_level(gp[i]) ? "high" : "low");
        printf("pin dial codes %s\n", dialplan_pins_active() ? "active" : "inactive (not one line + no microSD)");
        dialplan_pin_entry_t pe[DIALPLAN_MAX_PIN_ACTIONS];
        size_t np = dialplan_list_pin_entries(pe, DIALPLAN_MAX_PIN_ACTIONS);
        for (size_t i = 0; i < np; i++) {
            printf("  code %-6s -> GPIO%u, %s (%s)\n", pe[i].code, pe[i].gpio,
                   pe[i].secs ? "timed" : "toggle", pe[i].desc);
        }
        printf("usage: auxpin <18-21> <seconds high | 0 = toggle>\n");
        return 0;
    }
    int gpio = atoi(argv[1]), secs = atoi(argv[2]);
    esp_err_t err = auxpins_pulse(gpio, secs);
    if (err == ESP_ERR_INVALID_STATE) { printf("not driven: %s\n", why); return 1; }
    if (err != ESP_OK) { printf("auxpin: %s\n", esp_err_to_name(err)); return 1; }
    printf("GPIO%d is now %s\n", gpio, auxpins_level(gpio) ? "high" : "low");
    return 0;
}

/* webhang: freezes the web server on purpose to check the watchdog restarts
 * the board (~100 s). Console only. */
static int cmd_webhang(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("webhang yes -- blocks the web server forever, to test that its watchdog restarts\n"
               "the board within ~100 s. The web UI stops answering until then.\n");
        return 1;
    }
    webui_debug_hang();
    printf("web server blocked; expect 'webui watchdog' lines, then a restart in ~100 s\n");
    return 0;
}

static int cmd_board_info(int argc, char **argv)
{
    printf("board: smd\n"); /* kept for older bringup scripts that require the key */
    /* Exact PCB, and what this physical board last confirmed booting as --
     * flash_board.py reads these to pick the right build before reflashing. */
    char recorded[16];
    board_type_recorded(recorded, sizeof(recorded));
    printf("pcb: %s\n", board_pcb_variant_name());
    printf("pcb_recorded: %s\n", recorded[0] ? recorded : "-");
    printf("lines: %d\n", BOARD_NUM_LINES);
    printf("i2c: SDA=%d SCL=%d\n", BOARD_I2C_SDA, BOARD_I2C_SCL);
    /* Network state, so flash_board.py can say where to go next without
     * guessing an mDNS name. */
    char ap[33], host[80], ip[16];
    wifi_setup_ap_name(ap, sizeof(ap));
    wifi_live_hostname(host, sizeof(host));
    wifi_get_ip_str(ip, sizeof(ip));
    printf("wifi: %s\n", wifi_is_provisioning() ? "setup"
                         : wifi_is_connected() ? "connected" : "connecting");
    printf("setup_ap: %s\n", ap);
    printf("host: %s.local\n", host);
    printf("ip: %s\n", ip);
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

/* ---------- codec_init / dump (ES8388) ---------- */
static int cmd_codec_init(int argc, char **argv)
{
    es8388_config_t c = ES8388_DEFAULT_CONFIG();
    c.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    esp_err_t err = es8388_init(&c);
    printf("codec_init: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_codec_dump(int argc, char **argv)
{
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
    return 0;
}

/* ---------- micamp (ES8388 ADC PGA gain, live tuning) ---------- */
static int cmd_micamp(int argc, char **argv)
{
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
    if (argc < 3) {
        printf("usage: spkamp <left 0-33> <right 0-33>   (0=-45dB, 30=0dB, 33=+4.5dB, 1.5dB/step)\n"
               "current LOUT1VOL/ROUT1VOL = 0x%02X / 0x%02X\n",
               es8388_shadow(ES8388_REG_LOUT1VOL), es8388_shadow(ES8388_REG_ROUT1VOL));
        return 1;
    }
    int l = atoi(argv[1]), r = atoi(argv[2]);
    /* ES8388 datasheet (Reg. 46/47, LOUT1VOL/ROUT1VOL) only documents
     * 000000(0)=-45dB ... 011110(30)=0dB ... 100001(33)=+4.5dB, 1.5dB/step --
     * confirmed live 2026-09-25 that values above 33 are genuinely undefined
     * hardware behavior, not just "more gain": 35/36 gave plausible-looking
     * (if unverified) increasing output, but 37 and 40 produced a dead 0 Vpp
     * at the SLIC VIN pin -- an oscilloscope-confirmed cliff, not a mute
     * setting or a bad SLIC. The register is 6 bits wide (0-63 fits), but
     * the chip's internal gain-control DAC apparently isn't wired for
     * anything past the documented 34 codes. Reject out-of-range values
     * outright rather than silently writing into that territory again. */
    if (l < 0 || l > 33 || r < 0 || r > 33) {
        printf("left/right must each be 0-33 (0dB at 30) -- see ES8388 datasheet Reg. 46/47; "
               "values above 33 are undocumented and were confirmed live to cut output to 0\n");
        return 1;
    }
    esp_err_t err = es8388_set_dac_volume((uint8_t)l, (uint8_t)r);
    if (err != ESP_OK) {
        printf("write failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    /* Unlike micamp (diagnostic-only, deliberately never persisted -- see
     * its own comment), this is a real per-device setting a user tunes by
     * ear once and expects to keep -- same treatment as `aec delay`. */
    board_speaker_volume_set(0, (uint8_t)l);
    board_speaker_volume_set(1, (uint8_t)r);
    printf("LOUT1VOL=%d ROUT1VOL=%d (0dB at 30, 1.5dB/step, saved as default)\n", l, r);
    return 0;
}

/* ---------- spkhiz: force a line's LOUT/ROUT driver to high-Z ----------
 * Diagnostic for the BM64-loading problem (Bluetooth/BM64_AUDIO_DEBUG.md,
 * "AOHPL tap measurably loads and distorts Vin"): the ES8388's LOUT1/ROUT1
 * pins are active drivers even when "silent" (0V AC, not high-impedance),
 * which shunts the BM64's weaker analog output on the shared Vin node.
 * Two bits control this, and BOTH matter -- disabling one alone still
 * leaves the other:
 *   DACPOWER (Reg 4) bit5=LOUT1/bit4=ROUT1 -- output driver enable. Clearing
 *     it stops the driver actively holding the node...
 *   DACCONTROL23 (Reg 45) bit4=VROI -- 0=1.5k VREF-to-output (default), a
 *     real path to the shared node even with the driver disabled; 1=40k,
 *     a much lighter load. Confirmed against the real ES8388 datasheet.
 * VROI is a single global bit, not per-channel -- it only matters for a
 * disabled channel (an enabled driver dominates the node regardless), so
 * this only clears it back to the documented default once no line is left
 * in high-Z mode, rather than tracking it per-line. */
static uint8_t s_spkhiz_mask; /* which DACPOWER output-enable bits WE cleared (0x20=line0, 0x10=line1) */

static int cmd_spkhiz(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: spkhiz <line 0|1> <0|1>   (1 = force that line's codec output to "
               "high-Z -- LOUT1/ROUT1 disabled + VROI=40k -- so a companion signal on the "
               "shared Vin/Vout node isn't shunted by the codec's own driver)\n");
        return 1;
    }
    int line = atoi(argv[1]);
    if (line != 0 && line != 1) { printf("spkhiz: line must be 0 or 1\n"); return 1; }
    bool hiz = atoi(argv[2]) != 0;
    uint8_t bit = (line == 0) ? 0x20 /* LOUT1 */ : 0x10 /* ROUT1 */;

    uint8_t dacpower;
    if (es8388_read_reg(ES8388_REG_DACPOWER, &dacpower) != ESP_OK) {
        printf("spkhiz: DACPOWER read failed\n");
        return 1;
    }

    if (hiz) {
        dacpower &= (uint8_t)~bit;
        s_spkhiz_mask |= bit;
        es8388_write_reg(ES8388_REG_DACCONTROL23, 0x10); /* VROI=1 (40k) */
    } else {
        dacpower |= bit;
        s_spkhiz_mask &= (uint8_t)~bit;
        if (s_spkhiz_mask == 0) {
            es8388_write_reg(ES8388_REG_DACCONTROL23, 0x00); /* back to documented default */
        }
    }
    es8388_write_reg(ES8388_REG_DACPOWER, dacpower);

    printf("line %d: %s (DACPOWER=0x%02X, VROI=%s)\n", line,
           hiz ? "HIGH-Z" : "normal drive", dacpower,
           s_spkhiz_mask ? "40k" : "1.5k (default)");
    return 0;
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

/* ---------- fx / fxstat: voice effects, for diagnostics ----------
 * Effects normally start from a key pressed during a call. `fx` sets one by
 * hand and `fxstat` shows the levels going in and out of it, so "too loud" or
 * "distorted" can be seen in numbers. To hear yourself through an effect with
 * no call: `fx 0 1` then `loopback 0 1`, lift the handset and talk. */
static const char *FX_NAMES[8] = { "off", "Dalek", "chipmunk", "Vader", "8-bit", "robot",
                                   "tremolo", "distortion" };

static int cmd_fx(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: fx <line> <effect> [both]\n  effect: 0 off, 1 Dalek, 2 chipmunk, 3 Vader, "
               "4 8-bit, 5 robot, 6 tremolo, 7 distortion\n  both = also the other line (a two-phone call)\n"
               "  hear yourself with no call: fx 0 1 ; loopback 0 1   (hang up or `fx 0 0` to end)\n");
        return 1;
    }
    int line = atoi(argv[1]), fx = atoi(argv[2]);
    if (line < 0 || line >= AUDIO_NUM_LINES || fx < 0 || fx > 7) { printf("line 0-1, effect 0-7\n"); return 1; }
    audio_set_voice_fx(line, (voice_fx_t)fx);
    if (argc > 3 && strcmp(argv[3], "both") == 0) audio_set_voice_fx(1 - line, (voice_fx_t)fx);
    printf("line %d: effect %s\n", line, FX_NAMES[fx]);
    return 0;
}

static int cmd_fxgain(int argc, char **argv)
{
    if (argc > 1) {
        int pct = atoi(argv[1]);
        if (pct < 10 || pct > 300) { printf("usage: fxgain [percent 10-300]\n"); return 1; }
        voice_fx_set_gain_pct(pct);
    }
    printf("voice effect gain: %d%% (100 = level-matched default; resets on reboot)\n", voice_fx_gain_pct());
    return 0;
}

/* What each effect should do to a pure tone of `hz`: where the strongest line
 * lands. Returns NULL if it's right, otherwise what's wrong. */
static const char *fx_expect(voice_fx_t fx, float hz, const fx_tone_result_t *r)
{
    float want = hz, tol = fmaxf(12.0f, hz * 0.02f);
    switch (fx) {
    case VOICE_FX_DALEK: {
        /* 30 Hz ring modulation: two lines at hz-30 and hz+30, nothing at hz. */
        float m = r->peak_hz, o = r->second_hz;
        bool ok = (fabsf(m - (hz - 30)) < 12 && fabsf(o - (hz + 30)) < 12) ||
                  (fabsf(m - (hz + 30)) < 12 && fabsf(o - (hz - 30)) < 12);
        return ok ? NULL : "expected lines at tone +/- 30 Hz";
    }
    case VOICE_FX_CHIPMUNK: want = hz * 1.5f; tol = fmaxf(12.0f, want * 0.03f); break;
    case VOICE_FX_VADER:    want = hz * 0.8f; tol = fmaxf(12.0f, want * 0.03f); break;
    default: break;
    }
    if (fabsf(r->peak_hz - want) > tol) return "main line is at the wrong pitch";
    if (fx == VOICE_FX_TREMOLO && r->env_ratio > 0.5f) return "no amplitude wobble";
    return NULL;
}

static void fxtest_row(voice_fx_t fx, float hz, const fx_tone_result_t *r, bool digital)
{
    const char *bad = fx == VOICE_FX_NONE ? NULL : fx_expect(fx, hz, r);
    if (!bad && digital && r->limited) bad = "peak limiter had to act";
    if (!bad && r->clipped) bad = "clipped";
    char timing[24] = "";
    if (digital) snprintf(timing, sizeof(timing), "%4.0f/%4.0f us", r->us_avg, r->us_max);
    printf("  %-10s %6.0f Hz %6.1f | %6.0f Hz %6.1f | %6.1f %6.1f %5.1f%% %4.2f %s %s%s\n",
           voice_fx_name(fx), r->peak_hz, r->peak_dbfs, r->second_hz, r->second_dbfs,
           r->rms_dbfs, r->sample_peak_dbfs, r->other_pct, r->env_ratio, timing,
           bad ? "FAIL: " : "ok", bad ? bad : "");
}

typedef struct { voice_fx_t fx; float hz, amp; fx_tone_result_t r; esp_err_t e; volatile bool done; } fx_other_t;

static void fx_other_task(void *arg)
{
    fx_other_t *o = arg;
    o->e = audio_fx_roundtrip(1, o->fx, o->hz, o->amp, &o->r);
    o->done = true;
    vTaskDelete(NULL);
}

static int cmd_fxtest(int argc, char **argv)
{
    float hz = argc > 1 ? (float)atof(argv[1]) : 1000.0f;
    int line = argc > 2 ? atoi(argv[2]) : -1;
    int only = argc > 3 ? atoi(argv[3]) : -1;       /* one effect, repeated 3 times */
    bool both = argc > 4 && strcmp(argv[4], "both") == 0;   /* line 1 plays the same effect meanwhile */
    float amp = 0.3f;
    if (hz < 200 || hz > 2500 || line >= AUDIO_NUM_LINES) {
        printf("usage: fxtest [tone Hz 200-2500, default 1000] [line] [effect 0-7: just that one, x3] [both: line 1 runs it too]\n"
               "  Plays a pure tone through every effect and reports the two strongest lines out.\n"
               "  Without a line: in software only. With a line (on-hook!): also through the real\n"
               "  effect path, DAC, SLIC and back in through the hybrid and ADC.\n");
        return 1;
    }
    printf("tone %.0f Hz at %.1f dBFS peak (%.1f dBFS rms) in. Columns: strongest line (dBFS), "
           "2nd line, rms, peak, energy outside those two lines, 10 ms envelope min/max, "
           "time per 10 ms frame avg/max\n",
           hz, 20 * log10f(amp), 20 * log10f(amp / 1.4142f));
    printf("software:\n");
    for (int fx = (only >= 0 ? only : 0); fx <= (only >= 0 ? only : 7); fx++) {
        fx_tone_result_t r;
        esp_err_t e = audio_fx_tone_test((voice_fx_t)fx, hz, amp, &r);
        if (e != ESP_OK) { printf("  %-10s failed: %s\n", FX_NAMES[fx], esp_err_to_name(e)); continue; }
        fxtest_row((voice_fx_t)fx, hz, &r, true);
    }
    if (line >= 0) {
        printf("line %d round trip (earpiece level as set; the normal row is the line's own response):\n", line);
        for (int i = 0, fx = (only >= 0 ? only : 0); i < (only >= 0 ? 3 : 8); i++, fx += (only >= 0 ? 0 : 1)) {
            fx_tone_result_t r;
            fx_other_t *other = both ? calloc(1, sizeof(*other)) : NULL;
            if (other) {
                other->fx = (voice_fx_t)fx; other->hz = hz; other->amp = amp;
                xTaskCreate(fx_other_task, "fxother", 4096, other, 5, NULL);
            }
            esp_err_t e = audio_fx_roundtrip(line, (voice_fx_t)fx, hz, amp, &r);
            if (e != ESP_OK) { printf("  %-10s failed: %s\n", FX_NAMES[fx], esp_err_to_name(e)); }
            else fxtest_row((voice_fx_t)fx, hz, &r, false);
            if (other) {
                for (int w = 0; w < 100 && !other->done; w++) vTaskDelay(pdMS_TO_TICKS(100));
                if (other->done && other->e == ESP_OK) { printf("   (line 1)"); fxtest_row((voice_fx_t)fx, hz, &other->r, false); }
                else printf("   (line 1) failed: %s\n", other->done ? esp_err_to_name(other->e) : "timeout");
                if (other->done) free(other);
            }
        }
    }
    return 0;
}

static float fx_db(int32_t v) { return v > 0 ? 20.0f * log10f((float)v / 32768.0f) : -99.0f; }

static int cmd_fxstat(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 10;
    if (secs < 1 || secs > 120) { printf("usage: fxstat [secs 1-120, default 10]\n"); return 1; }
    printf("effect levels, dBFS (peak/rms in -> out). 'limited' = samples the peak limiter bent.\n");
    for (int t = 0; t < secs; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        for (int line = 0; line < AUDIO_NUM_LINES; line++) {
            for (int down = 1; down >= 0; down--) {
                voice_fx_stats_t s;
                audio_voice_fx_stats(line, down, &s);
                if (s.fx == VOICE_FX_NONE || !s.samples) continue;
                printf("  line %d %-6s %-10s in %5.1f/%5.1f -> out %5.1f/%5.1f  limited %lu of %lu\n",
                       line, down ? "hears" : "sends", voice_fx_name(s.fx),
                       fx_db(s.in_peak), fx_db((int32_t)s.in_rms),
                       fx_db(s.out_peak), fx_db((int32_t)s.out_rms),
                       (unsigned long)s.limited, (unsigned long)s.samples);
            }
        }
    }
    return 0;
}

/* ---------- reboot ----------
 * A software restart of the ESP32 only -- unlike reopening the serial port
 * (which toggles DTR/RTS and resets the chip), this keeps the monitor
 * session attached across the reboot. */
static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
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

/* ---------- download (reboot into the ROM's bootloader) ----------
 * Sets LP_AON_SYS_CFG_REG's force-download-boot bit and restarts, so the
 * ROM stops there instead of running the app. Two reasons this exists:
 *
 *  - On SMD5 with the clone MCU module, GPIO12/13 carry I2S MCLK and
 *    microSD card-detect, so ~1s into every boot the firmware takes them
 *    away from the native-USB PHY and the USB device disappears from the
 *    host -- there's no port left for esptool to open and reset. In the ROM
 *    they're USB again, and they stay that way.
 *  - On any board it saves reaching for the BOOT button.
 *
 * The analog side is quieted first: MCLK stops (or starts carrying USB
 * traffic) the moment the ROM takes over, and a codec running on a dead or
 * garbage clock can bang the SLIC's VIN node. DAC muted, both lines
 * silenced, ringers stopped, SLICs powered down -- the same state a
 * hung-up line sits in.
 *
 * The bit survives a software/hard reset, so the board keeps landing in
 * download mode until it loses power: **power-cycle when flashing is
 * done**. (BOOT + RESET does the same job with no firmware involved, and
 * is the escape hatch when firmware is too broken to reach this command.)
 */
static int cmd_download(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int l = 0; l < AUDIO_NUM_LINES; l++) {
        audio_tx_silence(l);
        ag1170_ring_stop(l);
        ag1170_set_power(l, false);
    }
    es8388_set_dac_mute(true);
    printf("entering download mode -- flash now; power-cycle the board when done\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    REG_SET_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    return 0;
}

/* ---------- miclevel (handset mic level / clipping check) ----------
 * Talk into the handset for a few seconds and see how hot the uplink is at
 * the codec ADC: peak dBFS, RMS, and how many samples hit (or nearly hit)
 * full scale. Line 0's SLIC VOUT feeds both the ES8388 LIN1 and (on BT
 * boards) the BM64 MIC_P1, so this is also the best proxy we have for what
 * the BM64's mic input sees -- the estimated volts are referred back to the
 * codec pin using the ES8388 ADC full scale (~1.0 Vrms at AVDD 3.3 V) and the
 * current PGA gain, ignoring any series-R/input-impedance divider in front.
 *
 * Uses the diag tap (not audio_rx_read()), so it's safe during a live call or
 * BT passthrough. It does silence the line's own TX first, because dial tone
 * leaks straight through the SLIC hybrid into VOUT and would swamp the
 * reading -- hang up and pick up again to get dial tone back afterwards. */
typedef struct {
    volatile uint32_t n;
    volatile uint32_t clip;       /* |x| >= 32767 (hard full scale) */
    volatile uint32_t near;       /* |x| >= -1 dBFS */
    volatile int32_t  peak;
    volatile uint64_t sumsq;
    volatile int32_t  win_peak;   /* per-print-window, reset by the console side */
    volatile uint64_t win_sumsq;
    volatile uint32_t win_n;
} miclevel_ctx_t;

#define MICLEVEL_NEAR_THRESH 29204   /* 32767 * 10^(-1/20) */

static void miclevel_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line;
    miclevel_ctx_t *m = ctx;
    int32_t pk = m->peak, wpk = m->win_peak;
    uint64_t ss = 0;
    uint32_t clip = 0, near = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = mono[i] < 0 ? -(int32_t)mono[i] : mono[i];
        if (a > pk) pk = a;
        if (a > wpk) wpk = a;
        if (a >= 32767) clip++;
        if (a >= MICLEVEL_NEAR_THRESH) near++;
        ss += (uint64_t)(a * a);
    }
    m->peak = pk;
    m->win_peak = wpk;
    m->sumsq += ss;
    m->win_sumsq += ss;
    m->n += n;
    m->win_n += n;
    m->clip += clip;
    m->near += near;
}

static float dbfs(double v)
{
    return v > 0 ? 20.0f * log10f((float)(v / 32768.0)) : -99.0f;
}

static int cmd_miclevel(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 5;
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    if (secs < 1 || secs > 60 || line < 0 || line >= AUDIO_NUM_LINES) {
        printf("usage: miclevel [secs 1-60, default 5] [line, default 0]\n");
        return 1;
    }
    uint8_t a1 = es8388_shadow(ES8388_REG_ADCCONTROL1);
    float pga_db = 3.0f * (float)((line == 0) ? (a1 >> 4) : (a1 & 0x0F));
    if (!ag1170_is_offhook(line)) {
        printf("note: line %d is on-hook -- pick up the handset or you'll only see the idle floor\n", line);
    }
    static miclevel_ctx_t m;
    memset((void *)&m, 0, sizeof(m));
    audio_tx_silence(line);
    audio_add_diag_tap(line, miclevel_tap, &m);
    printf("Line %d, PGA +%.0f dB. Talk normally, then loud, for %d s...\n", line, pga_db, secs);
    printf("   peak dBFS   rms dBFS\n");
    int steps = secs * 4;
    float loudest_rms = -99;
    for (int i = 0; i < steps; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int32_t wpk = m.win_peak;
        uint64_t wss = m.win_sumsq;
        uint32_t wn = m.win_n;
        m.win_peak = 0; m.win_sumsq = 0; m.win_n = 0;
        float pk = dbfs(wpk);
        float rms = wn ? dbfs(sqrt((double)wss / wn)) : -99;
        if (rms > loudest_rms) loudest_rms = rms;
        int bar = (int)((pk + 60.0f) / 2.0f);   /* 1 char per 2 dB, -60..0 dBFS */
        if (bar < 0) bar = 0;
        if (bar > 30) bar = 30;
        char b[32];
        memset(b, '#', bar);
        b[bar] = 0;
        printf("   %7.1f   %7.1f  |%-30s|%s\n", pk, rms, b, wpk >= 32767 ? " CLIP" : "");
    }
    audio_remove_diag_tap(line);

    if (m.n == 0) {
        printf("no samples captured (audio task not running?)\n");
        return 1;
    }
    double rms_all = sqrt((double)m.sumsq / m.n);
    float pk_db = dbfs(m.peak);
    /* ES8388 ADC: full-scale sine (peak 32767) ~= 1.0 Vrms at the pin with PGA 0 dB. */
    float gain = powf(10.0f, pga_db / 20.0f);
    float vpk_in = (float)m.peak / 32768.0f * 1.414f / gain;
    /* dBFS is re FS peak; a full-scale sine is -3 dBFS rms and ~1.0 Vrms. */
    float vrms_loud = powf(10.0f, (loudest_rms + 3.01f) / 20.0f) / gain;
    printf("\nSummary over %.1f s (%lu samples):\n", (double)m.n / audio_sample_rate(), (unsigned long)m.n);
    printf("  peak           %6.1f dBFS  (~%.0f mV peak at codec pin)\n", pk_db, vpk_in * 1000);
    printf("  rms (overall)  %6.1f dBFS\n", dbfs(rms_all));
    printf("  rms (loudest 0.25 s) %4.1f dBFS  (~%.0f mVrms at codec pin)\n", loudest_rms, vrms_loud * 1000);
    printf("  clipped        %lu samples (%.3f%%)\n", (unsigned long)m.clip, 100.0 * m.clip / m.n);
    printf("  within 1 dB FS %lu samples (%.3f%%)\n", (unsigned long)m.near, 100.0 * m.near / m.n);
    if (m.clip > 0) {
        printf("  -> CLIPPING: lower micamp (ADC PGA) for this line\n");
    } else if (pk_db > -3) {
        printf("  -> hot but not clipping: OK, little headroom\n");
    } else if (pk_db < -20) {
        printf("  -> quiet: raise micamp, or check the handset mic/VOUT path\n");
    } else {
        printf("  -> healthy level\n");
    }
    if (line == 0) {
        printf("  BM64 MIC_P1 sees roughly the same VOUT: its full scale is ~800 mVrms at min\n"
               "  mic gain, ~4 mVrms at max -- ~%.0f mVrms loud speech wants a low BM64 mic gain.\n",
               vrms_loud * 1000);
    }
    printf("(line TX was silenced for the test -- hang up/pick up to get dial tone back)\n");
    return 0;
}

/* ---------- tonetest: is the audio coming back clean? ----------
 * Plays a 350+450 Hz test burst on a line and analyzes what the SLIC returns
 * on its Vout (the same node the handset mic uses). The firmware's own tone is
 * clean, so anything found here was added after the DAC: clipping in the
 * SLIC's input stage, a too-hot codec output, or a bad coupling part. The
 * analysis itself lives in audio/tone_purity.c, shared with the web UI's
 * self-test. */
static int cmd_tonetest(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    if (line < 0 || line >= AUDIO_NUM_LINES) {
        printf("usage: tonetest [line, default 0] [tx]\n"
               "  tx = analyze the digital samples about to be sent to the codec instead of\n"
               "       what the SLIC returns, to tell firmware problems from analog ones\n");
        return 1;
    }
    bool digital = (argc > 2 && strcmp(argv[2], "tx") == 0);
    if (!digital && !ag1170_is_offhook(line)) {
        printf("note: line %d is on-hook; lifting the handset gives a realistic load\n", line);
    }
    tone_purity_t r;
    esp_err_t err = audio_tone_purity(line, digital, &r);
    if (err == ESP_ERR_NO_MEM) { printf("not enough free memory for the capture buffer\n"); return 1; }
    if (err == ESP_ERR_TIMEOUT) { printf("capture didn't complete (audio task stalled?)\n"); return 1; }

    printf("Line %d test tone, %s (0.50 s capture):\n", line,
           digital ? "digital samples going to the codec" : "as returned by the SLIC");
    printf("  peak  %6.1f dBFS   clipped samples: %lu\n", r.peak_dbfs, (unsigned long)r.clipped);
    printf("  350 Hz %7.1f dBFS    450 Hz %7.1f dBFS\n", r.tone_a_dbfs, r.tone_b_dbfs);
    printf("  strongest distortion products:\n");
    for (int i = 0; i < 3; i++) printf("    %4.0f Hz  %7.1f dBFS\n", r.top_hz[i], r.top_dbfs[i]);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("  -> no tone came back: line %d isn't powered or its Vout path is open\n", line);
        return 1;
    }
    printf("  distortion (listed products vs. tone): %.2f%%\n", r.distortion_pct);
    if (r.clipped > 0)             printf("  -> CLIPPING at the ADC: lower the codec output (spkamp)\n");
    else if (r.distortion_pct > 10) printf("  -> heavily distorted: clipped upstream of the ADC (SLIC input stage or codec output)\n");
    else if (r.distortion_pct > 3)  printf("  -> audibly impure: lower spkamp a step or two and re-run\n");
    else                            printf("  -> clean\n");
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
 * next one).
 *
 * No line number in the name (dropped 2026-09-14, matching telephony.c's
 * rec_name_timestamped() -- "rec_l<N>_..." reads as the digits "10" at a
 * glance, not "line 0"). record_capture()'s name_tag splicing below still
 * finds a second "_" to insert into for the wall-clock form (between date
 * and time), just not for the boot-relative fallback, which now has only
 * one "_" total -- a tagged capture taken in the first few seconds after a
 * fresh boot, before SNTP syncs, loses its "dl_"/etc tag until then. */
static void record_name_timestamped(char *out, size_t out_len)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        snprintf(out, out_len, "rec_boot%lld.wav",
                 (long long)(esp_timer_get_time() / 1000));
        return;
    }
    struct tm t;
    localtime_r(&now, &t);   /* local wall clock -- see the note on naming below */
    snprintf(out, out_len, "rec_%04d%02d%02d_%02d%02d%02d.wav",
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
#define ECHOPROBE_BUCKET_MS   5   /* plot resolution -- single-run mode only */
#define ECHOPROBE_SETTLE_MS 300   /* gap between repeats so one run's tail
                                    * (up to the full 400ms window, per what
                                    * DualPhone measured) can't bleed into the
                                    * next run's noise-floor sample */
#define ECHOPROBE_BAR_COLS   50   /* fixed absolute scale (of int16 full
                                    * scale), so bars are comparable run to
                                    * run and board to board, not just within
                                    * one capture */

/* Runs one probe via the shared audio_echo_probe() (also used by the webui
 * AEC calibration wizard, so the console and the web UI can never disagree
 * about what a measurement means). `plot` prints the 5ms-bucket RMS bar
 * chart (single-run mode); repeat mode skips it -- N of those would be
 * mostly noise to scroll past when what's actually wanted is whether the
 * numbers hold up run to run. */
static bool cmd_echoprobe_run(int line, uint32_t sr, size_t n_samples,
                               int16_t *cap, bool plot, audio_echo_probe_t *out)
{
    if (!audio_echo_probe(line, (uint32_t)ECHOPROBE_BURST_HZ, ECHOPROBE_BURST_MS,
                           ECHOPROBE_MS, cap, n_samples, out)) {
        printf("no audio captured\n");
        return false;
    }
    size_t got = n_samples; /* audio_echo_probe() fills up to n_samples; short
                              * captures just leave the tail as silence, which
                              * only affects the plot's trailing rows */

    if (plot) {
        printf("  noise floor rms=%u\n", (unsigned)out->noise_rms);
        printf("  peak %d at %.1f ms  <- bulk delay (DMA + codec + acoustic)\n",
               (int)out->peak, out->delay_ms);
        printf("  tail ~%.1f ms after peak\n", out->tail_ms);
        if (!out->leak_detected) {
            printf("  NOTE: peak barely above noise -- little/no local leak on this line,\n"
                   "        or the handset is on-hook. AEC would have nothing to cancel.\n");
        } else {
            /* Total span from t=0 (burst start), not just the post-peak
             * tail -- this is what the filter actually has to reach. */
            double span_ms = out->delay_ms + out->tail_ms;
            printf("  => AEC filter must span ~%.0f ms (%u taps @ 8 kHz)\n",
                   span_ms, (unsigned)((uint64_t)(span_ms * 8000.0 / 1000.0)));
        }

        /* Amplitude-vs-time plot, RMS per ECHOPROBE_BUCKET_MS bucket (RMS
         * rather than a raw per-sample peak so a single-sample glitch can't
         * spike one bar -- the point here is showing the envelope shape).
         * Fixed absolute bar scale (ECHOPROBE_BAR_COLS / 32767), not
         * normalized to this run's own peak, so it's meaningful to eyeball
         * against a different run or a different board. */
        printf("  envelope (%d ms/row, rms, fixed scale):\n", ECHOPROBE_BUCKET_MS);
        size_t bucket_n = (size_t)((uint64_t)sr * ECHOPROBE_BUCKET_MS / 1000);
        if (bucket_n < 1) bucket_n = 1;
        char bar[ECHOPROBE_BAR_COLS + 1];
        for (size_t b0 = 0; b0 < got; b0 += bucket_n) {
            size_t b1 = b0 + bucket_n; if (b1 > got) b1 = got;
            uint64_t bacc = 0;
            for (size_t i = b0; i < b1; i++) bacc += (uint64_t)((int32_t)cap[i] * cap[i]);
            uint32_t brms = (uint32_t)sqrt((double)bacc / (double)(b1 - b0));
            int cols = (int)(((uint64_t)brms * ECHOPROBE_BAR_COLS) / 32767);
            if (cols > ECHOPROBE_BAR_COLS) cols = ECHOPROBE_BAR_COLS;
            memset(bar, '#', cols);
            bar[cols] = '\0';
            printf("  %5.1f ms  rms=%5u |%s\n",
                   (double)b0 * 1000.0 / (double)sr, (unsigned)brms, bar);
        }
    }
    return true;
}

static int cmd_echoprobe(int argc, char **argv)
{
    int line = (argc > 1) ? atoi(argv[1]) : 0;
    int reps = (argc > 2) ? atoi(argv[2]) : 1;
    if (line < 0 || line > 1) { printf("usage: echoprobe [line] [# repeats]\n"); return 1; }
    if (reps < 1) reps = 1;

    uint32_t sr = audio_sample_rate();
    size_t n_samples = (size_t)((uint64_t)sr * ECHOPROBE_MS / 1000);

    int16_t *cap = malloc(n_samples * sizeof(int16_t));
    if (!cap) { printf("malloc failed\n"); return 1; }

    printf("echoprobe line %d: %.0f Hz burst %d ms, listening %d ms, %d run%s...\n",
           line, ECHOPROBE_BURST_HZ, ECHOPROBE_BURST_MS, ECHOPROBE_MS,
           reps, reps > 1 ? "s" : "");

    int hits = 0;
    double delay_sum = 0, delay_min = 1e9, delay_max = -1e9;
    double tail_sum = 0, tail_min = 1e9, tail_max = -1e9;
    int ok_runs = 0;

    for (int r = 0; r < reps; r++) {
        if (reps > 1) printf("run %d/%d:\n", r + 1, reps);
        audio_echo_probe_t res;
        if (!cmd_echoprobe_run(line, sr, n_samples, cap, reps == 1, &res)) continue;
        ok_runs++;
        if (reps > 1) {
            printf("  noise=%u  peak=%d  delay=%.1fms  tail=%.1fms%s\n",
                   (unsigned)res.noise_rms, (int)res.peak, res.delay_ms, res.tail_ms,
                   res.leak_detected ? "" : "  (no leak above noise)");
        }
        if (res.leak_detected) {
            hits++;
            delay_sum += res.delay_ms;
            if (res.delay_ms < delay_min) delay_min = res.delay_ms;
            if (res.delay_ms > delay_max) delay_max = res.delay_ms;
            tail_sum += res.tail_ms;
            if (res.tail_ms < tail_min) tail_min = res.tail_ms;
            if (res.tail_ms > tail_max) tail_max = res.tail_ms;
        }
        if (r + 1 < reps) vTaskDelay(pdMS_TO_TICKS(ECHOPROBE_SETTLE_MS));
    }

    if (reps > 1) {
        printf("---\n");
        if (ok_runs == 0) {
            printf("no successful captures\n");
        } else if (hits == 0) {
            printf("leak detected in 0/%d runs -- no repeatable echo above noise floor\n", ok_runs);
        } else {
            printf("leak detected in %d/%d runs -- %s\n", hits, ok_runs,
                   hits == ok_runs ? "consistent, real echo" :
                   "intermittent -- background noise may be contaminating some runs");
            printf("  delay: min=%.1fms max=%.1fms mean=%.1fms\n",
                   delay_min, delay_max, delay_sum / hits);
            printf("  tail:  min=%.1fms max=%.1fms mean=%.1fms\n",
                   tail_min, tail_max, tail_sum / hits);
        }
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
        /* A human deliberately typing this (unlike aec_sweep_try()'s rapid
         * internal probing, which calls aec_set_delay_ms() directly and
         * never touches NVS) means "make this the default" -- each board's
         * bulk delay is its own device-specific property (see
         * board_aec_delay_get()'s comment), so persist it. */
        board_aec_delay_set(line, ms);
        printf("line %d: AEC bulk delay %u ms (filter reset, saved as default)\n", line, (unsigned)ms);
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
               "anyway, but NOT saved as the default (not a confident result). Try again.\n",
               best_delay, best_erle);
    } else {
        board_aec_delay_set(line, (uint32_t)best_delay);
        printf("Best: %d ms (ERLE %d dB). Delay set, filter reset, saved as default.\n",
               best_delay, best_erle);
    }
    printf("AEC is left ON for line %d -- it does NOT turn off on hangup. "
           "Remember to `aec off %d` when you're done testing.\n", line, line);
    return 0;
}

typedef void (*tap_add_fn)(int line, audio_rx_tap_fn fn, void *ctx);
typedef void (*tap_remove_fn)(int line);

/* Shared by `record` (mic/uplink) and `record_downlink` (network/downlink)
 * below -- identical in every way except which tap API captures the
 * samples, so that's the only thing parameterized. `name_tag` is spliced
 * into the generated filename, between the date and time segments (empty
 * string for plain `record`) so the two are easy to tell apart in the
 * storage list without inventing a whole second naming/eviction category --
 * these still start with "rec_" so is_protected_asset() treats them as
 * ordinary, evictable recordings like any other, not permanent assets. */
static int record_capture(int ms, int line, const char *name_tag,
                           tap_add_fn add_tap, tap_remove_fn remove_tap)
{
    if (ms <= 0 || line < 0 || line > 1) { printf("bad args\n"); return 1; }

    uint32_t sr = audio_sample_rate();
    size_t n_samples = (size_t)((int64_t)sr * ms / 1000);
    size_t expected_bytes = 44 + n_samples * sizeof(int16_t);

    char name[STORAGE_MAX_NAME];
    record_name_timestamped(name, sizeof(name));
    if (name_tag && name_tag[0]) {
        /* Splice the tag in between the date and time segments --
         * record_name_timestamped()'s wall-clock form ("rec_YYYYMMDD_HHMMSSZ.wav")
         * has exactly one "_" before that point once "rec" itself is
         * skipped; the boot-relative fallback ("rec_bootNNN.wav") has none,
         * so a tag silently doesn't get spliced in during the first few
         * seconds after a fresh boot, before SNTP syncs. */
        char *insert_at = strchr(name, '_');
        insert_at = insert_at ? strchr(insert_at + 1, '_') : NULL;
        if (insert_at) {
            char rest[STORAGE_MAX_NAME];
            strncpy(rest, insert_at + 1, sizeof(rest) - 1);
            rest[sizeof(rest) - 1] = '\0';
            snprintf(insert_at + 1, sizeof(name) - (size_t)(insert_at + 1 - name),
                     "%s%s", name_tag, rest);
        }
    }

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
    add_tap(line, record_tap, &rc);

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

    remove_tap(line);
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

/* record_capture() blocks for the whole capture duration (ms), which used
 * to mean it ran directly on the console REPL task -- so the console
 * couldn't accept ANY further input (not even Enter) until it finished.
 * Fine for a standalone `record` where nothing else needs typing during
 * the capture, but it's a hard blocker for anything that needs a SECOND
 * command run on the SAME console while the first is still recording (e.g.
 * `record_downlink` then `play` on the same board, for a local audio-
 * quality test loop -- confirmed live: the console was completely
 * unresponsive for the whole 20s, `play` could never be typed in time).
 * Both commands now spawn record_capture() on its own task and return
 * immediately instead, so the console stays usable; the "saved ..."
 * (or failure) message still prints, just asynchronously once it's done. */
typedef struct {
    int ms, line;
    char name_tag[8];
    tap_add_fn add_tap;
    tap_remove_fn remove_tap;
} record_task_args_t;

static void record_task(void *arg)
{
    record_task_args_t *a = arg;
    record_capture(a->ms, a->line, a->name_tag, a->add_tap, a->remove_tap);
    free(a);
    vTaskDelete(NULL);
}

static bool record_start(int ms, int line, const char *name_tag,
                          tap_add_fn add_tap, tap_remove_fn remove_tap)
{
    record_task_args_t *a = malloc(sizeof(*a));
    if (!a) { printf("malloc failed\n"); return false; }
    a->ms = ms;
    a->line = line;
    strncpy(a->name_tag, name_tag, sizeof(a->name_tag) - 1);
    a->name_tag[sizeof(a->name_tag) - 1] = '\0';
    a->add_tap = add_tap;
    a->remove_tap = remove_tap;
    if (xTaskCreate(record_task, "record", 4096, a, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        printf("xTaskCreate failed\n");
        free(a);
        return false;
    }
    printf("recording line %d for %d ms in the background -- console still usable, "
           "\"saved ...\" prints here when it's done\n", line, ms);
    return true;
}

static int cmd_record(int argc, char **argv)
{
    if (argc < 2) { printf("usage: record <ms> [line]\n"); return 1; }
    int ms = atoi(argv[1]);
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    return record_start(ms, line, "", audio_add_diag_tap, audio_remove_diag_tap) ? 0 : 1;
}

/* Captures what's about to be PLAYED on a line -- for an active call, the
 * decoded network audio, before it ever reaches the DAC/earpiece. No
 * acoustic path involved (unlike `record`, which taps the mic), so this is
 * what actually isolates a network/decode/DMA dropout from ordinary room
 * noise or echo -- see audio_add_downlink_tap()'s doc comment in audio.h.
 * Also what to use on the RECEIVING end of a two-board audio-quality test
 * (play a known clip from one board, `record_downlink` on the other, and
 * compare); or on the ORIGINATING board after enabling `txrx_loopback 1`
 * on the far end mid-call, which echoes whatever it receives straight back
 * -- letting you capture the full round trip from a single board (this
 * runs in the background specifically so that single board's console is
 * still free to run `play` right after starting the capture). */
static int cmd_record_downlink(int argc, char **argv)
{
    if (argc < 2) { printf("usage: record_downlink <ms> [line]\n"); return 1; }
    int ms = atoi(argv[1]);
    int line = (argc > 2) ? atoi(argv[2]) : 0;
    return record_start(ms, line, "dl_", audio_add_downlink_tap, audio_remove_downlink_tap) ? 0 : 1;
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
    int64_t t0 = esp_timer_get_time();
    int prev0 = -1, prev1 = -1;
    bool prev_off0 = false, prev_off1 = false;
    for (int i = 0; i < secs * 50; i++) {
        int r0 = ag1170_raw_shk(0), r1 = ag1170_raw_shk(1);
        bool off0 = ag1170_is_offhook(0), off1 = ag1170_is_offhook(1);
        if (r0 != prev0 || r1 != prev1 || off0 != prev_off0 || off1 != prev_off1) {
            long long t_ms = (long long)((esp_timer_get_time() - t0) / 1000);
            printf("  t=%6lldms  raw SHK: L0=%d(%s) L1=%d(%s)\n", t_ms,
                   r0, off0 ? "off" : "on",
                   r1, off1 ? "off" : "on");
            prev0 = r0; prev1 = r1;
            prev_off0 = off0; prev_off1 = off1;
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
/* How long the DTMF detector takes per 10 ms frame (it runs for each line that's
 * off-hook, inside the audio task, so this is time taken from the 10 ms budget). */
static int cmd_audiostat(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int l = 0; l < AUDIO_NUM_LINES; l++)
        printf("line %d: external audio underruns since boot: %u frames\n", l, (unsigned)audio_ext_underruns(l));
    return 0;
}

static int cmd_dtmfbench(int argc, char **argv)
{
    (void)argc; (void)argv;
    static dtmf_detector_t d;
    uint32_t fs = audio_sample_rate();
    size_t n = fs / 100;
    int16_t *frame = malloc(n * sizeof(int16_t));
    if (!frame) { printf("no memory\n"); return 1; }
    dtmf_detector_init(&d, fs);
    uint32_t r = 12345;
    int64_t sum = 0, worst = 0;
    const int frames = 200;
    for (int f = 0; f < frames; f++) {
        for (size_t i = 0; i < n; i++) { r = r * 1664525u + 1013904223u; frame[i] = (int16_t)((int32_t)(r >> 17) - 16384) / 4; }
        int64_t t0 = esp_timer_get_time();
        dtmf_detector_push(&d, frame, n);
        int64_t dt = esp_timer_get_time() - t0;
        sum += dt;
        if (dt > worst) worst = dt;
        if ((f & 15) == 15) vTaskDelay(1);
    }
    printf("dtmf detector: %lld us average, %lld us worst per %u-sample (10 ms) frame\n",
           (long long)(sum / frames), (long long)worst, (unsigned)n);
    free(frame);
    return 0;
}

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

/* ---------- btcall: HF call control / state, for testing ring-and-answer ---------- */
static int cmd_btcall(int argc, char **argv)
{
    static const char *names[] = { "idle", "voice dial", "incoming", "outgoing",
                                   "active", "active+waiting", "active+hold" };
    if (argc > 1) {
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (strcmp(argv[1], "accept") == 0)      err = bt_companion_accept_call();
        else if (strcmp(argv[1], "reject") == 0) err = bt_companion_reject_call();
        else if (strcmp(argv[1], "end") == 0)    err = bt_companion_end_call();
        if (err != ESP_OK) { printf("btcall: %s\n", esp_err_to_name(err)); return 1; }
    }
    bt_companion_status_t st = bt_companion_get_status();
    unsigned cs = (unsigned)st.call_state;
    printf("BT call: %s%s%s\n", cs < 7 ? names[cs] : "?",
           st.caller_id[0] ? ", caller " : "", st.caller_id);
    return 0;
}

/* ---------- power: low-power idle state ----------
 * Unlike the web UI, the console doesn't count as activity, so this can
 * actually observe the board asleep. */
static int cmd_power(int argc, char **argv)
{
#if CONFIG_PM_ENABLE
    /* `power locks`: which drivers are holding the clock up right now. A
     * CPU-frequency readout would be useless here -- FreeRTOS holds the
     * clock at max while any task runs, including this one. Rebuild with
     * CONFIG_PM_PROFILING=y to also get time spent at each clock. */
    if (argc > 1 && strcmp(argv[1], "locks") == 0) {
        esp_pm_dump_locks(stdout);
        return 0;
    }
#endif
    static const char *names[] = { "disabled", "awake (line in use)", "awake, idle", "ASLEEP" };
    idle_power_status_t st = idle_power_status();
    printf("low-power: %s", names[st.state]);
    if (st.state == IDLE_POWER_COUNTING) printf(", sleeps in %lu s", (unsigned long)st.secs_until_sleep);
    if (st.state == IDLE_POWER_ASLEEP)   printf(" for %lu s", (unsigned long)st.secs_asleep_now);
    printf("\n  %lu sleeps since boot, %lu s asleep of %lu s up; last sleep %lu s%s\n",
           (unsigned long)st.sleep_count, (unsigned long)st.total_asleep_s,
           (unsigned long)st.uptime_s, (unsigned long)st.last_sleep_s,
           st.woken_by_web ? " (ended by a web request)" : "");
    printf("  audio pipeline %s, WiFi TX %d dBm (RSSI %d dBm)\n",
           st.audio_paused ? "PAUSED" : "running", st.tx_power_dbm, st.rssi_dbm);
    return 0;
}

/* ---------- txpower: WiFi transmit power, for testing adaptive TX ----------
 * A manual value sticks until the adaptive loop next decides to *change*
 * level, so this is for trying out a link at reduced power, not a setting. */
static int cmd_txpower(int argc, char **argv)
{
    if (argc > 1) {
        esp_err_t err = wifi_set_tx_power_dbm(atoi(argv[1]));
        if (err != ESP_OK) { printf("txpower: %s\n", esp_err_to_name(err)); return 1; }
    }
    printf("WiFi TX power %d dBm, RSSI %d dBm\n", wifi_get_tx_power_dbm(), wifi_rssi());
    return 0;
}

/* ---------- selftest: guided hardware bring-up check ---------- */

static const char *selftest_result_str(selftest_result_t r)
{
    switch (r) {
        case SELFTEST_PASS: return "PASS";
        case SELFTEST_FAIL: return "FAIL";
        case SELFTEST_SKIP: return "skip";
        default:            return "----";
    }
}

/* Takes a borrowed pointer, never a copy -- selftest_report_t is ~1.2 KB and
 * the console REPL task only has a 4 KB stack (a by-value copy here faulted
 * it outright on hardware). */
static void selftest_print(const selftest_report_t *rep)
{
    for (int i = 0; i < rep->n; i++) {
        const selftest_check_t *c = &rep->checks[i];
        printf("  %-9s %-4s %-32s %s\n", c->id, selftest_result_str(c->result),
               c->title, c->detail);
    }
}

static int cmd_selftest(int argc, char **argv)
{
    /* Sub-commands mirror the web UI's flow so either can drive a run:
     * no arg = run the automated checks and list what's left for a human. */
    if (argc > 2 && strcmp(argv[1], "answer") == 0) {
        bool ok = (argc > 3) && (atoi(argv[3]) != 0);
        esp_err_t err = selftest_answer(argv[2], ok);
        if (err != ESP_OK) {
            printf("selftest: no such check '%s' (or it's skipped on this board)\n", argv[2]);
            return 1;
        }
        printf("selftest: %s = %s\n", argv[2], ok ? "PASS" : "FAIL");
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "action") == 0) {
        printf("waiting up to 15s -- do the thing now...\n");
        selftest_result_t r = selftest_run_action(argv[2], 15000);
        const selftest_report_t *rep = selftest_report();
        const selftest_check_t *c = NULL;
        for (int i = 0; i < rep->n; i++) {
            if (strcmp(rep->checks[i].id, argv[2]) == 0) c = &rep->checks[i];
        }
        printf("selftest: %s = %s%s%s\n", argv[2], selftest_result_str(r),
               (c && c->detail[0]) ? " -- " : "", (c && c->detail[0]) ? c->detail : "");
        return (r == SELFTEST_FAIL) ? 1 : 0;
    }
    if (argc > 2 && strcmp(argv[1], "stimulus") == 0) {
        esp_err_t err = selftest_stimulus(argv[2]);
        printf("selftest stimulus %s: %s\n", argv[2],
               err == ESP_OK ? "done" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "save") == 0) {
        esp_err_t err = selftest_history_append();
        printf("selftest: %s\n", err == ESP_OK ? "run recorded to this board's NVS"
                                               : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "history") == 0) {
        selftest_run_t hist[SELFTEST_HISTORY_MAX];
        size_t n = selftest_history_get(hist, SELFTEST_HISTORY_MAX);
        if (n == 0) { printf("no recorded runs on this board yet\n"); return 0; }
        printf("recorded runs (newest first):\n");
        for (size_t i = 0; i < n; i++) {
            char when[32] = "date unknown";
            if (hist[i].unix_time) {
                time_t t = (time_t)hist[i].unix_time;
                struct tm tmv;
                gmtime_r(&t, &tmv);
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &tmv);
            }
            printf("  %-20s %u pass, %u fail, %u skip, %u unanswered\n", when,
                   hist[i].n_pass, hist[i].n_fail, hist[i].n_skip, hist[i].n_pending);
        }
        return 0;
    }

    char id[16] = {0};
    selftest_board_id(id, sizeof(id));
    selftest_begin();
    printf("board id %s -- automated checks done, human steps still pending:\n", id);
    selftest_print(selftest_report());
    printf("\nnext: selftest action <id>   (physical step, firmware verifies)\n"
           "      selftest stimulus <id>  (make it ring, etc)\n"
           "      selftest answer <id> <0|1>\n"
           "      selftest save           (record this run on the board)\n");
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

/* The networks from the most recent wifi_scan, numbered, so wifi_set can take
 * `#3` instead of a typed name. Typing is where special characters get lost:
 * a curly apostrophe (iPhones put one in the hotspot name), an accented
 * letter or an emoji doesn't make it through a serial terminal reliably, and
 * the result is a different network name that simply isn't found. */
#define SCAN_CACHE_MAX 32
static char s_scan_ssid[SCAN_CACHE_MAX][33];
static int  s_scan_n;

/* In-place %XX decoding, for `wifi_set -u`. */
static void pct_decode(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Letters and digits only, lower-cased: "Scotts iPhone" and "Scott’s iPhone"
 * both become "scottsiphone". */
static void name_key(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    for (; *in && n + 1 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c)) out[n++] = (char)tolower(c);
    }
    out[n] = '\0';
}

static int cmd_wifi_set(int argc, char **argv)
{
    int a = 1;
    bool url_encoded = false;
    if (argc > 1 && strcmp(argv[1], "-u") == 0) { url_encoded = true; a = 2; }
    if (argc - a < 2) {
        printf("usage: wifi_set [-u] <ssid|#N> <password>\n"
               "  wifi_set \"My Network\" \"my password\"      (quote values with spaces)\n"
               "  wifi_set #3 \"my password\"                  (network #3 from the last wifi_scan:\n"
               "                                               use this for names with an apostrophe,\n"
               "                                               accent or emoji)\n"
               "  wifi_set -u \"Caf%%C3%%A9\" \"p%%22ss%%25\"      (-u: %%XX escapes, any bytes;\n"
               "                                               write a literal %% as %%25)\n");
        return 1;
    }

    static char ssid[64], pass[64];
    int next = collect_quoted_or_plain(argc, argv, a, ssid, sizeof(ssid));
    if (next < 0) { printf("wifi_set: unterminated ' in SSID\n"); return 1; }
    next = collect_quoted_or_plain(argc, argv, next, pass, sizeof(pass));
    if (next < 0) { printf("wifi_set: unterminated ' in password\n"); return 1; }
    if (url_encoded) { pct_decode(ssid); pct_decode(pass); }

    if (ssid[0] == '#') {
        int n = atoi(ssid + 1);
        if (s_scan_n == 0) { printf("wifi_set: run wifi_scan first, then use its #numbers\n"); return 1; }
        if (n < 1 || n > s_scan_n) { printf("wifi_set: no #%d in the last scan (1-%d)\n", n, s_scan_n); return 1; }
        strlcpy(ssid, s_scan_ssid[n - 1], sizeof(ssid));
        printf("using #%d from the last scan: '%s'\n", n, ssid);
    } else if (s_scan_n > 0) {
        /* Not an exact name in the last scan? Often the right network under a
         * slightly different spelling -- say so before waiting 40 s. */
        bool exact = false;
        int near = -1;
        char want[40], have[40];
        name_key(ssid, want, sizeof(want));
        for (int i = 0; i < s_scan_n; i++) {
            if (strcmp(s_scan_ssid[i], ssid) == 0) { exact = true; break; }
            name_key(s_scan_ssid[i], have, sizeof(have));
            if (want[0] && strcmp(want, have) == 0 && near < 0) near = i;
        }
        if (!exact && near >= 0) {
            printf("note: '%s' is not in the last scan, but '%s' (#%d) looks like it.\n"
                   "      Try:  wifi_set #%d <password>\n", ssid, s_scan_ssid[near], near + 1, near + 1);
        }
    }
    {
        bool non_ascii = false;
        for (const char *c = ssid; *c; c++) if ((unsigned char)*c >= 0x80) non_ascii = true;
        if (non_ascii) {
            printf("SSID bytes:");
            for (const char *c = ssid; *c; c++) printf(" %02X", (unsigned char)*c);
            printf("\n");
        }
    }
    size_t plen = strlen(pass);
    if (plen > 0 && plen < 8) {
        printf("note: a WPA2/WPA3 password is 8-63 characters; this one is %u\n", (unsigned)plen);
    }

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
        char ip[16], host[64];
        wifi_get_ip_str(ip, sizeof(ip));
        board_hostname(host, sizeof(host));
        printf("connected! IP=%s -- try http://%s/ or http://%s.local/ from a browser "
               "on the same network.\n", ip, ip, host);
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

static int cmd_wifi_list(int argc, char **argv)
{
    wifi_known_net_t nets[WIFI_MAX_NETWORKS];
    int n = wifi_known_list(nets, WIFI_MAX_NETWORKS);
    if (n == 0) { printf("no saved networks\n"); return 0; }
    for (int i = 0; i < n; i++) {
        printf("  '%s'%s%s\n", nets[i].ssid, nets[i].has_password ? "" : "  (open)",
               strcmp(nets[i].ssid, wifi_get_ssid()) == 0 ? "  <- current" : "");
    }
    return 0;
}

static const char *auth_name(uint8_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
    default:                        return "other";
    }
}

static int cmd_wifi_scan(int argc, char **argv)
{
    wifi_scan_result_t *r = calloc(32, sizeof(*r));
    if (!r) { printf("out of memory\n"); return 1; }
    int64_t t0 = esp_timer_get_time();
    int n = wifi_scan(r, 32);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (n < 0) {
        printf("scan failed (%d ms) -- see the wifi_app warning above\n", ms);
    } else {
        printf("%d network(s) in %d ms%s\n", n, ms,
               wifi_is_provisioning() ? " (setup AP is up)" : "");
        s_scan_n = 0;
        for (int i = 0; i < n; i++) {
            printf("  #%-2d %4d dBm  %-6s %s\n", i + 1, r[i].rssi, auth_name(r[i].authmode), r[i].ssid);
            if (i < SCAN_CACHE_MAX) {
                strlcpy(s_scan_ssid[i], r[i].ssid, sizeof(s_scan_ssid[i]));
                s_scan_n = i + 1;
            }
        }
        if (n > 0) printf("use a name from this list with: wifi_set #<number> <password>\n");
    }
    free(r);
    return n < 0;
}

static int cmd_wifi_forget(int argc, char **argv)
{
    if (argc < 2) { printf("usage: wifi_forget <ssid>  (see wifi_list)\n"); return 1; }
    esp_err_t err = wifi_known_remove(argv[1]);
    printf("wifi_forget '%s': %s\n", argv[1],
           err == ESP_OK ? "removed" : err == ESP_ERR_NOT_FOUND ? "not a saved network"
                                                                 : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
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
               "  e.g. sip_set sip.example.com phone <pass> phone2 <pass2>\n"
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
    return sdcard_selftest(NULL, NULL) == ESP_OK ? 0 : 1;
}

/* ---------- BM64 bring-up ---------- */
static int cmd_bm64_test(int argc, char **argv)
{
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 2000;
    int baud = (argc > 2) ? atoi(argv[2]) : 0;
    esp_err_t err = bt_companion_raw_ping_at_baud(timeout_ms, baud);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("bt_companion is already enabled (its own poll task is running) -- disable it "
               "first (Settings -> Bluetooth, or bt_companion_set_enabled(false)) before "
               "running this raw test, they can't share the UART at once\n");
        return 1;
    }
    printf("bm64_test: %s\n", err == ESP_OK ? "got a valid reply -- module is alive on this UART"
                                             : "no valid reply -- see the log line above for what to check");
    return err == ESP_OK ? 0 : 1;
}

static int cmd_bm64_addr(int argc, char **argv)
{
    int timeout_ms = (argc > 1) ? atoi(argv[1]) : 2000;
    char addr[18];
    esp_err_t err = bt_companion_read_bd_addr(timeout_ms, addr, sizeof(addr));
    if (err == ESP_ERR_INVALID_STATE) {
        printf("bt_companion is already enabled (its own poll task is running) -- disable it "
               "first before running this raw test, they can't share the UART at once\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("bm64_addr: no reply -- see the log line above\n");
        return 1;
    }
    printf("bm64_addr: %s\n", addr);
    return 0;
}

static int cmd_bm64_snoop(int argc, char **argv)
{
    int duration_ms = (argc > 1) ? atoi(argv[1]) * 1000 : 20000;
    esp_err_t err = bt_companion_raw_snoop(duration_ms);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("bt_companion is already enabled (its own poll task is running) -- disable it "
               "first (bt_enable 0) before running this raw test, they can't share the UART at once\n");
        return 1;
    }
    return err == ESP_OK ? 0 : 1;
}

/* Parses a hex string like "04 00 1D 07 00 00" or "0400 1D07 0000" (spaces
 * optional/anywhere) into raw bytes. Returns the byte count, or -1 on a
 * malformed string (odd number of hex digits). */
static int parse_hex_bytes(const char *s, uint8_t *out, size_t out_max)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        if (*s == ' ' || *s == ',') continue;
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else return -1;
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= out_max) return -1;
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return (hi < 0) ? (int)n : -1; /* hi>=0 here means a trailing lone nibble */
}

static int cmd_bm64_raw(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bm64_raw <hex bytes, e.g. \"04 00 1D 07 00 00\"> [listen_ms, default 500] "
               "-- sends raw bytes with NO AA00 framing, for probing BM64's undocumented IBDK/\n"
               "test-mode EEPROM protocol. Does NOT touch MFB/RST_N/P2_0 -- set those yourself "
               "with bm64_pin first (IBDK mode needs p2_0 held low). bt_companion must be "
               "disabled.\n");
        return 1;
    }
    uint8_t tx[64];
    int tx_len = parse_hex_bytes(argv[1], tx, sizeof(tx));
    if (tx_len <= 0) {
        printf("bm64_raw: couldn't parse '%s' as hex bytes (need an even number of hex digits)\n", argv[1]);
        return 1;
    }
    int listen_ms = (argc > 2) ? atoi(argv[2]) : 500;
    esp_err_t err = bt_companion_raw_send(tx, (size_t)tx_len, listen_ms);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("bt_companion is already enabled (its own poll task is running) -- disable it "
               "first (bt_enable 0) before running this raw test, they can't share the UART at once\n");
        return 1;
    }
    return err == ESP_OK ? 0 : 1;
}

/* ---------- bm64_ibdk / bm64_eeprom / bm64_config ----------
 * Thin wrappers now: the IBDK entry sequence, HCI init and the ACL-based
 * EEPROM read/write all live in bt_companion so the web UI can run the
 * config check too (see bt_companion.h's "IBDK mode + EEPROM access"). */

static int cmd_bm64_ibdk(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "exit") == 0) {
        esp_err_t err = bt_companion_ibdk_exit();
        printf("bm64_ibdk exit: %s\n", err == ESP_OK
               ? "BM64 reset to normal mode -- use bt_enable 1 to restart bt_companion"
               : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    /* "init" re-runs only the HCI handshake, for a module already sitting in
     * IBDK mode (or with the mode pins driven by hand via bm64_pin). */
    bool toggle_pins = !(argc > 1 && strcmp(argv[1], "init") == 0);
    esp_err_t err = bt_companion_ibdk_enter(toggle_pins);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("bm64_ibdk: disable bt_companion first (bt_enable 0), and this board's "
               "config must be single-line with no microSD\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("bm64_ibdk: failed (%s) -- see the log above for which step\n",
               esp_err_to_name(err));
        return 1;
    }
    printf("IBDK init complete. Use bm64_eeprom to read/write EEPROM.\n");
    return 0;
}

static int cmd_bm64_eeprom(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: bm64_eeprom read  <hex_addr> [len, default 1]\n"
               "       bm64_eeprom write <hex_addr> <hex_byte>\n"
               "  e.g. bm64_eeprom read 0348\n"
               "       bm64_eeprom write 0348 0A\n"
               "  Run bm64_ibdk first to enter IBDK mode and init the HCI link.\n");
        return 1;
    }

    bool is_write = (strcmp(argv[1], "write") == 0 || strcmp(argv[1], "w") == 0);
    bool is_read  = (strcmp(argv[1], "read") == 0  || strcmp(argv[1], "r") == 0);
    if (!is_read && !is_write) {
        printf("bm64_eeprom: first arg must be 'read' or 'write'\n");
        return 1;
    }
    if (!bt_companion_ibdk_ready()) {
        printf("bm64_eeprom: run bm64_ibdk first\n");
        return 1;
    }

    uint16_t addr = (uint16_t)strtoul(argv[2], NULL, 16);

    if (is_read) {
        unsigned long len = (argc > 3) ? strtoul(argv[3], NULL, 10) : 1;
        if (len == 0 || len > 256) { printf("bm64_eeprom: len must be 1-256\n"); return 1; }
        uint8_t data[256];
        esp_err_t err = bt_companion_eeprom_read(addr, data, len);
        if (err != ESP_OK) {
            printf("EEPROM READ failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("0x%04X:", addr);
        for (unsigned long i = 0; i < len; i++) printf(" %02X", data[i]);
        printf("\n");
        return 0;
    }

    if (argc < 4) {
        printf("bm64_eeprom write: need a data byte, e.g. bm64_eeprom write 0348 0A\n");
        return 1;
    }
    uint8_t value = (uint8_t)strtoul(argv[3], NULL, 16);
    esp_err_t err = bt_companion_eeprom_write(addr, value);
    if (err != ESP_OK) {
        printf("EEPROM WRITE failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("0x%04X: wrote 0x%02X OK\n", addr, value);
    return 0;
}

/* Checks (and with 'apply', repairs) the handful of EEPROM bytes this board
 * needs for analog audio out of the BM64. Handles the whole disruptive
 * sequence itself -- parks the poll task, enters IBDK, restores normal mode
 * -- so unlike bm64_ibdk/bm64_eeprom it does NOT want bt_enable 0 first. */
static int cmd_bm64_config(int argc, char **argv)
{
    bool apply = (argc > 1 && (strcmp(argv[1], "apply") == 0 || strcmp(argv[1], "fix") == 0));
    printf("%s BM64 EEPROM config -- Bluetooth drops for ~15s...\n",
           apply ? "Checking and repairing" : "Checking");

    bm64_config_report_t rep;
    esp_err_t err = bt_companion_config_check(apply, &rep);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        printf("bm64_config: this board has no GPIO control of the BM64 mode pins\n");
        return 1;
    }

    if (rep.bd_addr[0]) printf("module BD address: %s\n", rep.bd_addr);
    for (int i = 0; i < rep.n_items; i++) {
        const bm64_config_item_t *it = &rep.items[i];
        printf("  0x%04X: got %02X want %02X  %-8s %s\n",
               it->addr, it->found, it->expected,
               it->written ? "WRITTEN" : (it->matched ? "ok" : "MISMATCH"),
               it->what ? it->what : "");
    }
    if (rep.error[0]) {
        printf("bm64_config: %s\n", rep.error);
        return 1;
    }
    if (rep.ok) {
        printf("bm64_config: all %d bytes correct%s\n", rep.n_items,
               rep.n_written ? " (after writing)" : "");
    } else {
        printf("bm64_config: %d of %d byte(s) still wrong -- run 'bm64_config apply' to fix\n",
               rep.n_mismatched, rep.n_items);
    }
    return 0;
}

static int cmd_bm64_mmi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bm64_mmi <hex action code, e.g. 51> -- bt_companion must be enabled and "
               "link up (unlike bm64_test/bm64_addr/bm64_snoop, this one needs it ON)\n");
        return 1;
    }
    uint8_t action = (uint8_t)strtol(argv[1], NULL, 16);
    esp_err_t err = bt_companion_send_mmi(action);
    if (err != ESP_OK) {
        printf("bm64_mmi: failed (bt_companion not enabled, or link not up)\n");
        return 1;
    }
    printf("bm64_mmi: sent MMI action 0x%02X -- watch the log for the ACK\n", action);
    return 0;
}

static int cmd_device_name(int argc, char **argv)
{
    if (argc < 2) {
        char name[32] = {0};
        board_name_get(name, sizeof(name));
        printf("device name: '%s'\n", name);
        return 0;
    }
    esp_err_t err = board_name_set(argv[1]);
    if (err != ESP_OK) {
        printf("device_name: failed to set (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("device name set to '%s' (takes effect on next reboot)\n", argv[1]);
    return 0;
}

static int cmd_bm64_pair(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = bt_companion_start_pairing();
    if (err != ESP_OK) {
        printf("bm64_pair: failed (%s) -- bt_companion must be enabled and link up\n",
               esp_err_to_name(err));
        return 1;
    }
    char name[32] = {0};
    board_name_get(name, sizeof(name));
    printf("bm64_pair: pairing started as '%s' -- LED should blink blue, check your phone\n", name);
    return 0;
}

static int cmd_bm64_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bm64_cmd <hex opcode> [param bytes...] -- e.g.\n"
               "         bm64_cmd 0E                     (Read_Paired_Device_Record, no params)\n"
               "         bm64_cmd 23 00 01 03 0F 0F 00    (Set_Overall_Gain: db0, A2DP+HF mask, "
               "abs level, A2DP=15, HF=15, line_in unused)\n"
               "       No param bytes sends a single 0x00 reserved byte, matching the plain "
               "opcode-only real command set entries.\n"
               "       bt_companion must be enabled and link up; reply shows up as an "
               "'unhandled event' hex dump in the log at debug level (log_level debug first)\n");
        return 1;
    }
    uint8_t opcode = (uint8_t)strtol(argv[1], NULL, 16);
    uint8_t params[254];
    uint8_t n = 0;
    if (argc > 2) {
        for (int i = 2; i < argc && n < sizeof(params); i++) {
            params[n++] = (uint8_t)strtol(argv[i], NULL, 16);
        }
    } else {
        params[n++] = 0x00; /* matches the old bm64_cmd's fixed single reserved byte */
    }
    esp_err_t err = bt_companion_send_cmd(opcode, params, n);
    if (err != ESP_OK) {
        printf("bm64_cmd: failed (bt_companion not enabled, or link not up)\n");
        return 1;
    }
    printf("bm64_cmd: sent opcode 0x%02X (%u param byte%s) -- watch the log (debug level) for "
           "the reply\n", opcode, n, n == 1 ? "" : "s");
    return 0;
}

/* Bring-up/bench only: bit-banged GPIO control of a BM64 module's MFB/RST_N/
 * P2_0 pins, for boards that have wired one to the spare GPIO18-20 pads
 * (see board_pins_smd4.h's BOARD_BT_MFB_PIN/_RST_PIN/_P20_PIN comment). Not
 * safe on a real 2-line or microSD-equipped board -- refuses unless this
 * board's runtime config says single-line, no SD, matching the same guard
 * ag1170.c/sdcard.c use before touching these same shared pads. */
#ifdef BOARD_BT_MFB_PIN   /* SMD4/SMD5 pin maps only -- SMD2 has none */
static int cmd_bm64_pin(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: bm64_pin <mfb|rst|p20> <0|1> -- only usable on a single-line, "
               "no-microSD board with BM64 bench-wired to GPIO18-20 (see board_pins_smd4.h)\n");
        return 1;
    }
    gpio_num_t pin;
    bool needs_shared_pads = true;   /* GPIO18-20: line 1's SLIC / microSD */
    if (strcmp(argv[1], "mfb") == 0) {
        pin = BOARD_BT_MFB_PIN;
#ifdef BOARD_BT_MFB_DEDICATED
        needs_shared_pads = false;   /* SMD5: MFB has a pin of its own */
#endif
    }
    else if (strcmp(argv[1], "rst") == 0) pin = BOARD_BT_RST_PIN;
    else if (strcmp(argv[1], "p20") == 0) pin = BOARD_BT_P20_PIN;
    else { printf("bm64_pin: unknown pin '%s' (want mfb, rst, or p20)\n", argv[1]); return 1; }

    board_hw_config_t cfg = board_hw_config_get();
    if (needs_shared_pads && (cfg.num_lines != 1 || cfg.has_microsd)) {
        printf("bm64_pin: refusing -- this board's config is %d-line%s, GPIO18-20 are claimed "
               "by line 2 SLIC/microSD, not free for BM64 control\n",
               cfg.num_lines, cfg.has_microsd ? " with microSD" : "");
        return 1;
    }

    /* Full gpio_config() with pull-up/down and interrupts explicitly
     * disabled -- matches ag1170.c's config_line_gpio(), the proven pattern
     * for this project's other bench-configured GPIO outputs (the SLIC's
     * FR/RM pins). Confirmed 2026-09-25: a bare gpio_set_direction() +
     * gpio_set_level() pair (what this command used to do) leaves pull_up_en
     * at whatever it last was, and a leftover internal pull-up fighting the
     * output was the actual cause of a full night of "MFB won't hold high"
     * debugging that looked exactly like a wiring/connection fault (see
     * archive/BM64_MFB_GPIO_DEBUG.md). gpio_sleep_sel_dis()/gpio_hold_dis()
     * are cheap insurance, not the fix -- keep them anyway so this pin's
     * state can't be disturbed if this board ever does enter real light
     * sleep for some other reason. */
    gpio_hold_dis(pin);
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);
    gpio_sleep_sel_dis(pin);
    int level = atoi(argv[2]) != 0;
    gpio_set_level(pin, level);
    printf("bm64_pin: %s (GPIO%d) set %s\n", argv[1], pin, level ? "HIGH" : "LOW");
    return 0;
}
#endif

/* ---------- BK3266 bring-up ----------
 * For the BK3266SR module on J2 (archive/hardware/bk3266.md has the
 * sequence). All of these except bk_state/bk_cmd need bt_companion disabled
 * first (bt_enable 0), because they own the UART directly. */

static int bk_busy(void)
{
    printf("the BK3266 driver is running (bt_enable 1). Use bt_enable 0 first, "
           "or bk_state / bk_cmd / bk_frame / bk_send / bk_at, which work alongside it.\n");
    return 1;
}

static int bk_check_module(void)
{
    if (bt_companion_module() == BOARD_BT_BK3266) return 0;
    printf("this board isn't set to a BK3266 module (Settings -> Hardware, then reboot). "
           "Raw bk_* commands still talk on GPIO16/17 at 9600 baud, so go ahead if that's "
           "what's wired.\n");
    return 0;   /* informational only: bring-up is done before the setting is changed */
}

static int cmd_bk_probe(int argc, char **argv)
{
    (void)argc; (void)argv;
    bk_check_module();
    esp_err_t err = bk3266_probe();
    if (err == ESP_ERR_INVALID_STATE) return bk_busy();
    return err == ESP_OK ? 0 : 1;
}

static int cmd_bk_baudscan(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = bk3266_baudscan();
    if (err == ESP_ERR_INVALID_STATE) return bk_busy();
    return err == ESP_OK ? 0 : 1;
}

static int cmd_bk_listen(int argc, char **argv)
{
    int ms = (argc > 1) ? atoi(argv[1]) : 5000;
    if (ms < 100) ms = 100;
    if (ms > 120000) ms = 120000;
    esp_err_t err = bk3266_listen(ms);
    if (err == ESP_ERR_INVALID_STATE) return bk_busy();
    return err == ESP_OK ? 0 : 1;
}

static int cmd_bk_send(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bk_send <hex bytes> [listen_ms, default 1000]\n"
               "  raw bytes, no framing added -- e.g. a complete frame:\n"
               "  bk_send \"52 42 23 04 01 14 19 23 42 52\"\n");
        return 1;
    }
    uint8_t buf[64];
    int n = parse_hex_bytes(argv[1], buf, sizeof(buf));
    if (n <= 0) { printf("bk_send: couldn't parse '%s' as hex bytes\n", argv[1]); return 1; }
    esp_err_t err = bk3266_send_raw(buf, (size_t)n, argc > 2 ? atoi(argv[2]) : 1000);
    if (err == ESP_ERR_INVALID_STATE) return bk_busy();
    return err == ESP_OK ? 0 : 1;
}

/* bk_cmd <code> [listen_ms]: one MCU->module command code in a proper frame
 * (cmd 0x01, DATA = code, checksum added). With the driver running it just
 * sends and the reply lands in the log / bk_state. */
static int cmd_bk_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bk_cmd <hex code> [listen_ms, default 1000]\n"
               "  01 pair   03 disconnect   04 answer   05 reject   06 hang up   07 redial\n"
               "  11 address  12 name  13 phone name  14 version  15 ACL?  16 call status\n"
               "  25 receive mode   (full table: components/bt_companion/include/bk3266_proto.h)\n");
        return 1;
    }
    uint8_t code[1];
    if (parse_hex_bytes(argv[1], code, 1) != 1) { printf("bk_cmd: one hex byte please\n"); return 1; }
    if (bk3266_running()) {
        esp_err_t e = bk3266_send_code(code[0]);
        printf("bk_cmd: sent %02X (%s); watch the log or run bk_state\n", code[0], esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    esp_err_t err = bk3266_send_framed(BK_MID_SEND, code, 1, argc > 2 ? atoi(argv[2]) : 1000);
    return err == ESP_OK ? 0 : 1;
}

/* bk_frame <cmd hex> [data hex...] [listen_ms]: any outer CMD, DATA, with
 * LEN/CRC/header/footer computed -- for trying the other reading of the
 * protocol (command code as the outer CMD). */
static int cmd_bk_frame(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bk_frame <cmd hex> [data hex bytes] [listen_ms]\n"
               "  bk_frame 14          (version query, code as CMD, no data)\n"
               "  bk_frame 01 \"14\"     (version query, code as DATA)\n");
        return 1;
    }
    uint8_t cmd[1], data[BK_FRAME_MAX_DATA];
    if (parse_hex_bytes(argv[1], cmd, 1) != 1) { printf("bk_frame: cmd must be one hex byte\n"); return 1; }
    int n = 0;
    if (argc > 2) {
        n = parse_hex_bytes(argv[2], data, sizeof(data));
        if (n < 0) { printf("bk_frame: couldn't parse data\n"); return 1; }
    }
    esp_err_t err = bk3266_send_framed(cmd[0], data, (size_t)n, argc > 3 ? atoi(argv[3]) : 1000);
    if (err == ESP_ERR_INVALID_STATE) return bk_busy();
    return err == ESP_OK ? 0 : 1;
}

/* bk_at <text> [listen_ms]: a text line + CRLF, for the text-AT firmware
 * variant (BT+..., COM+...). */
static int cmd_bk_at(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bk_at <text> [listen_ms, default 1500]   e.g. bk_at BT+PR\n");
        return 1;
    }
    char line[80];
    int n = snprintf(line, sizeof(line), "%s\r\n", argv[1]);
    if (n <= 0 || n >= (int)sizeof(line)) { printf("bk_at: too long\n"); return 1; }
    esp_err_t err = bk3266_send_raw((const uint8_t *)line, (size_t)n, argc > 2 ? atoi(argv[2]) : 1500);
    if (err == ESP_ERR_INVALID_STATE) return bk_busy();
    return err == ESP_OK ? 0 : 1;
}

static int cmd_bk_state(int argc, char **argv)
{
    (void)argc; (void)argv;
    bk3266_print_state();
    return 0;
}

static int cmd_bk_wake(int argc, char **argv)
{
    int ms = (argc > 1) ? atoi(argv[1]) : 300;
    esp_err_t err = bk3266_wake_pulse(ms);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        printf("bk_wake: WAKEUP is only on its own GPIO (GPIO15) on SMD5 boards\n");
        return 1;
    }
    if (err == ESP_ERR_INVALID_STATE) return bk_busy();
    printf("bk_wake: GPIO15 high for %d ms, then released. A short pulse is a button press; "
           "6 s would power the module off.\n", ms);
    return err == ESP_OK ? 0 : 1;
}

/* Runs the parser and state mapping on built-in frames, on the device itself.
 * Needs no module; a quick "is this build's protocol code sane". The full
 * set is firmware/test/host/test_bk3266_proto.c. */
static int cmd_bk_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const uint8_t frames[] = {
        0x52,0x42,0x23,0x04,0x00,0x64,0x68,0x23,0x42,0x52,   /* booted */
        0x52,0x42,0x23,0x04,0x00,0x65,0x69,0x23,0x42,0x52,   /* link established */
        0x52,0x42,0x23,0x04,0x00,0x73,0x77,0x23,0x42,0x52,   /* incoming call */
    };
    bk_parser_t p; bk_state_t st;
    bk_parser_init(&p); bk_state_init(&st);
    int good = 0;
    for (size_t i = 0; i < sizeof(frames); i++) {
        bk_frame_t f;
        if (bk_parser_feed(&p, frames[i], &f)) { bk_state_apply(&st, &f); good++; }
    }
    uint8_t out[BK_FRAME_MAX_BYTES];
    size_t n = bk_build_command(BK_CMD_MATCH, out, sizeof(out));
    static const uint8_t want[] = { 0x52,0x42,0x23,0x04,0x01,0x01,0x06,0x23,0x42,0x52 };
    bool build_ok = n == sizeof(want) && memcmp(out, want, n) == 0;
    bool ok = good == 3 && st.booted && st.connected && st.call == BK_CALL_INCOMING && build_ok;
    printf("bk_selftest: %d/3 frames parsed, state %s, pairing frame %s -> %s\n", good,
           (st.booted && st.connected && st.call == BK_CALL_INCOMING) ? "ok" : "WRONG",
           build_ok ? "ok" : "WRONG", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_bt_enable(int argc, char **argv)
{
    if (argc < 2) {
        printf("bt_companion is currently %s\n", bt_companion_enabled() ? "enabled" : "disabled");
        printf("usage: bt_enable <0|1>\n");
        return 0;
    }
    bool enabled = atoi(argv[1]) != 0;
    esp_err_t err = bt_companion_set_enabled(enabled);
    printf("bt_companion %s\n", err == ESP_OK ? (enabled ? "enabled" : "disabled") : "FAILED to change");
    return err == ESP_OK ? 0 : 1;
}

/* Compact help: one line per command ("cmd  description"), instead of the
 * built-in esp_console help command's 2-line-plus-blank-line-per-command
 * format. We already have every (command, help) pair at registration time,
 * so just stash them here rather than pulling them back out of esp_console
 * (commands.c's registry is static/internal, not exposed for enumeration). */
#define MAX_HELP_ENTRIES 64
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

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void console_task(void *arg)
{
    const char *prompt = arg;
    setvbuf(stdin, NULL, _IONBF, 0);
    printf("\r\nType 'help' to get the list of commands.\r\n");
    linenoiseSetMaxLineLen(128);
    while (1) {
        if (!usb_serial_jtag_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        char *line = linenoise(prompt);
        if (!line) {
            vTaskDelay(pdMS_TO_TICKS(20)); /* never spin, whatever stdin does */
            continue;
        }
        if (line[0]) linenoiseHistoryAdd(line);
        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }
}
#endif

/* gpiotest <gpio>: is something driving this pin? Reads it with the internal
 * pull-down, then the pull-up. A pin that follows the pull is floating; one
 * that reads the same both ways is being driven (high or low) from outside.
 * For a UART RX line: idle-high from a powered module reads high both times. */
static int cmd_gpiotest(int argc, char **argv)
{
    if (argc < 2) { printf("usage: gpiotest <gpio>   (bt_enable 0 first for GPIO16/17)\n"); return 1; }
    int pin = atoi(argv[1]);
    if (pin < 0 || pin > 23) { printf("gpiotest: pin out of range\n"); return 1; }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(20));
    int with_pd = gpio_get_level(pin);
    io.pull_up_en = GPIO_PULLUP_ENABLE; io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(20));
    int with_pu = gpio_get_level(pin);
    printf("GPIO%d: %d with pull-down, %d with pull-up -> %s\n", pin, with_pd, with_pu,
           with_pd == with_pu ? (with_pd ? "driven HIGH from outside" : "driven LOW from outside")
                              : "floating (nothing drives it)");
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io);
    return 0;
}

/* hwcfg: the console's version of Settings -> Hardware, for a board that
 * isn't on Wi-Fi yet. hwcfg [bt none|bm64|bk3266] [lines 1|2] [sd 0|1] */
static int cmd_hwcfg(int argc, char **argv)
{
    board_hw_config_t cfg = board_hw_config_get();
    bool change = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "bt")) {
            if (!strcmp(v, "bk3266")) cfg.bt_module = BOARD_BT_BK3266;
            else if (!strcmp(v, "bm64")) cfg.bt_module = BOARD_BT_BM64;
            else if (!strcmp(v, "none")) cfg.bt_module = BOARD_BT_NONE;
            else { printf("hwcfg: bt wants none, bm64 or bk3266\n"); return 1; }
            cfg.has_bt_module = cfg.bt_module != BOARD_BT_NONE;
        } else if (!strcmp(k, "lines")) {
            cfg.num_lines = atoi(v);
        } else if (!strcmp(k, "sd")) {
            cfg.has_microsd = atoi(v) != 0;
        } else {
            printf("hwcfg: unknown '%s' (want bt, lines or sd)\n", k);
            return 1;
        }
        change = true;
    }
    if (change) {
        esp_err_t err = board_hw_config_set(cfg);
        if (err != ESP_OK) { printf("hwcfg: refused (%s)\n", esp_err_to_name(err)); return 1; }
        printf("hwcfg: saved; reboot to apply\n");
    }
    cfg = board_hw_config_get();
    printf("lines %d, microSD %s, Bluetooth module %s, config confirmed %s\n", cfg.num_lines,
           cfg.has_microsd ? "yes" : "no",
           cfg.bt_module == BOARD_BT_BM64 ? "BM64" : cfg.bt_module == BOARD_BT_BK3266 ? "BK3266" : "none",
           board_hw_config_is_set() ? "yes" : "no");
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
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    repl_cfg.task_stack_size = 1536; /* its task never runs: see console_task() */
#endif

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
    register_cmd("bk_probe", "BK3266: is anything answering? listens, sends a version query", cmd_bk_probe);
    register_cmd("bk_baudscan", "BK3266: version query (binary + text) at every common baud rate", cmd_bk_baudscan);
    register_cmd("bk_listen", "BK3266: print raw bytes + decoded frames: bk_listen [ms]", cmd_bk_listen);
    register_cmd("bk_cmd", "BK3266: send one command code: bk_cmd <hex> [listen_ms]", cmd_bk_cmd);
    register_cmd("bk_frame", "BK3266: build + send a frame: bk_frame <cmd> [data] [ms]", cmd_bk_frame);
    register_cmd("bk_send", "BK3266: send raw hex bytes: bk_send <hex> [listen_ms]", cmd_bk_send);
    register_cmd("bk_at", "BK3266: send a text line (AT firmware): bk_at <text>", cmd_bk_at);
    register_cmd("bk_state", "BK3266: show decoded link/call/name state", cmd_bk_state);
    register_cmd("bk_wake", "BK3266: pulse WAKEUP (GPIO15, SMD5): bk_wake [ms]", cmd_bk_wake);
    register_cmd("bk_selftest", "BK3266: check the protocol code on built-in frames", cmd_bk_selftest);
    register_cmd("auxpin", "Spare pins GPIO18-21 (one line, no microSD): auxpin [<gpio> <secs|0 toggle>]", cmd_auxpin);
    register_cmd("webhang", "Test: freeze the web server to check its watchdog restarts the board", cmd_webhang);
    register_cmd("webpass", "Web UI password: webpass [clear] -- clear removes a forgotten one", cmd_webpass);
    register_cmd("i2c_scan", "Scan I2C bus for the codec", cmd_i2c_scan);
    register_cmd("codec_detect", "Probe both known codec addresses, warn on board-type mismatch", cmd_codec_detect);
    register_cmd("codec_init", "Initialize the ES8388 codec", cmd_codec_init);
    register_cmd("codec_dump", "Dump codec shadow registers", cmd_codec_dump);
    register_cmd("micamp", "ES8388 ADC PGA gain (SMD2): micamp <left 0-8> <right 0-8>, 3dB/step", cmd_micamp);
    register_cmd("spkamp", "ES8388 earpiece drive level: spkamp <left 0-33> <right 0-33>, 1.5dB/step, 30=0dB", cmd_spkamp);
    register_cmd("spkhiz", "Force a line's codec output to high-Z (for BT companion loading tests): spkhiz <line 0|1> <0|1>", cmd_spkhiz);

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
    register_cmd("reboot", "Software restart of the ESP32 (keeps the monitor attached)", cmd_reboot);
    register_cmd("download", "Reboot into the ROM bootloader for flashing (quiets audio/SLIC first; power-cycle when done)", cmd_download);
    register_cmd("fx", "Voice effect for diagnostics: fx <line> <0-7> [both] (see usage)", cmd_fx);
    register_cmd("fxtest", "Deterministic voice-effect check: a pure tone through each effect, spectrum out: fxtest [Hz] [line]", cmd_fxtest);
    register_cmd("fxgain", "Voice effect output gain for testing: fxgain [percent 10-300, default 100]", cmd_fxgain);
    register_cmd("fxstat", "Live levels through the voice effects: fxstat [secs]", cmd_fxstat);
    register_cmd("tonetest", "Dial-tone purity as returned by the SLIC (clipping/distortion): tonetest [line]", cmd_tonetest);
    register_cmd("miclevel", "Handset mic level/clipping check while you talk: miclevel [secs] [line]", cmd_miclevel);
    register_cmd("echopulse", "Periodic 1kHz burst for echo/hybrid debugging: echopulse <line> <on|off> [period_ms]", cmd_echopulse);
    register_cmd("echosweep", "Cycles 9 tones 300-3400Hz for echo/resonance debugging: echosweep <line> <on|off> [period_ms]", cmd_echosweep);
    register_cmd("record", "Raw mic capture to storage, no live playback, safe during an active call: record <ms> [line]", cmd_record);
    register_cmd("record_downlink", "Capture what's about to be played (network audio for a call), before the DAC -- no acoustic path: record_downlink <ms> [line]", cmd_record_downlink);
    register_cmd("aec", "Echo canceller: aec <on|off|status|reset|sweep|delay <ms>> [line] "
                        "(sweep needs a live call with the far end talking)", cmd_aec);
    register_cmd("echoprobe", "Measure the local earpiece->mic echo path (bulk delay + tail) for AEC sizing: echoprobe [line] [# repeats]", cmd_echoprobe);
    register_cmd("play", "Play a storage WAV out a line: play <name> [line]", cmd_play);
    register_cmd("dtmf_gen", "Emit DTMF: dtmf_gen <digits> [line]", cmd_dtmf_gen);
    register_cmd("slic", "SLIC pin: slic <line> <pd|fr|rm> <0|1>", cmd_slic);
    register_cmd("ring", "Ring a line: ring [line] [secs]", cmd_ring);
    register_cmd("hookmon", "Watch SHK: hookmon [secs]", cmd_hookmon);
    register_cmd("pulsemon", "Watch raw pulse dialing + timing: pulsemon [secs] [line]", cmd_pulsemon);
    register_cmd("audiostat", "Underruns in clip/VoIP playback since boot (dropouts)", cmd_audiostat);
    register_cmd("dtmfbench", "Time the DTMF detector per 10 ms audio frame", cmd_dtmfbench);
    register_cmd("tonemon", "Watch DTMF (touch-tone) digits + timing: tonemon [secs] [line]", cmd_tonemon);
    register_cmd("txpower", "WiFi transmit power: txpower [dBm 2-20] -- for testing; the adaptive-TX setting may change it back", cmd_txpower);
    register_cmd("btcall", "Bluetooth phone call: btcall [accept|reject|end] -- no arg shows call state and caller ID", cmd_btcall);
    register_cmd("power", "Low-power idle state (the console, unlike the web UI, doesn't wake the board)", cmd_power);
    register_cmd("selftest", "Guided hardware bring-up check: selftest | action <id> | stimulus <id> | answer <id> <0|1> | save | history -- results stored on this board", cmd_selftest);
    register_cmd("status", "Per-line telephony status", cmd_status);
    register_cmd("reset", "Force line(s) out of a stuck tone/reorder state: reset [line]", cmd_reset);
    register_cmd("wifi_set", "Save WiFi credentials + reconnect: wifi_set [-u] <ssid|#N> <password>", cmd_wifi_set);
    register_cmd("wifi_status", "WiFi connection status", cmd_wifi_status);
    register_cmd("wifi_list", "List saved WiFi networks", cmd_wifi_list);
    register_cmd("wifi_scan", "Scan for nearby WiFi networks", cmd_wifi_scan);
    register_cmd("wifi_forget", "Remove a saved WiFi network: wifi_forget <ssid>", cmd_wifi_forget);
    register_cmd("log_level", "Get/set global log level: log_level [none|error|warn|info|debug|verbose]", cmd_log_level);
    register_cmd("testmode", "Enable/disable telephony injection test mode: testmode <0|1>", cmd_testmode);
    register_cmd("tinject", "Inject handset events (test mode): tinject <offhook|onhook|digit|dial> [line] [d]", cmd_tinject);
    register_cmd("txrx_loopback", "Audio TX->RX digital loopback for tests: txrx_loopback <0|1>", cmd_txrx_loopback);
    register_cmd("sip_set", "Save SIP config: sip_set <server> <user0> <pass0> [user1] [pass1]", cmd_sip_set);
    register_cmd("sip_status", "SIP registration status", cmd_sip_status);
    register_cmd("sip_call", "Test SIP INVITE (signaling only): sip_call <number> [line]", cmd_sip_call);
    register_cmd("sip_hangup", "Hang up a SIP call: sip_hangup [line]", cmd_sip_hangup);
    register_cmd("sd_test", "microSD: mount (if needed), print card info, write/read/verify", cmd_sd_test);
    register_cmd("bm64_test", "Raw BM64 UART ping: bm64_test [timeout_ms, default 2000] [baud, default 115200] -- disable bt_companion first", cmd_bm64_test);
    register_cmd("bm64_addr", "Read BM64's own Bluetooth MAC address: bm64_addr [timeout_ms, default 2000] -- disable bt_companion first", cmd_bm64_addr);
    register_cmd("bm64_snoop", "Live raw-byte dump of the BM64 UART, no framing: bm64_snoop [seconds, default 20] -- disable bt_companion first", cmd_bm64_snoop);
    register_cmd("bm64_raw", "Send raw hex bytes with NO AA00 framing, print the reply: bm64_raw <hex bytes> [listen_ms, default 500] -- for probing BM64's undocumented IBDK/EEPROM protocol; doesn't touch MFB/RST_N/P2_0, set those with bm64_pin first; disable bt_companion first", cmd_bm64_raw);
    /* Registered unconditionally now -- bt_companion returns
     * ESP_ERR_NOT_SUPPORTED with a clear message on boards whose pin map has
     * no BM64 mode pins, which beats the command silently not existing. */
    register_cmd("bm64_ibdk", "IBDK mode: bm64_ibdk [init|exit] -- no arg = full pin toggle + HCI init; 'init' = HCI only (manual pins); 'exit' = reset to normal BT mode", cmd_bm64_ibdk);
    register_cmd("bm64_eeprom", "Read/write BM64 EEPROM: bm64_eeprom read <hex_addr> [len] | bm64_eeprom write <hex_addr> <hex_byte> -- BM64 must be in IBDK mode (bm64_ibdk)", cmd_bm64_eeprom);
    register_cmd("bm64_config", "Check (or 'bm64_config apply' to repair) the BM64 EEPROM bytes needed for analog audio -- handles IBDK entry/exit itself, no bt_enable 0 needed", cmd_bm64_config);
    register_cmd("device_name", "Get/set device name (BT + mDNS): device_name [new_name] -- persists to NVS, takes effect on reboot", cmd_device_name);
    register_cmd("bt_enable", "Get/set bt_companion enabled: bt_enable [0|1]", cmd_bt_enable);
    register_cmd("bm64_pair", "Start BT pairing (discoverable mode): bm64_pair -- bt_companion must be enabled and link up", cmd_bm64_pair);
    register_cmd("bm64_mmi", "Send a raw MMI action code: bm64_mmi <hex, e.g. 51> -- bt_companion must be ENABLED (opposite of bm64_test/addr/snoop)", cmd_bm64_mmi);
    register_cmd("bm64_cmd", "Send a real command-set command with hex param bytes (e.g. 23 00 01 03 0F 0F 00 for Set_Overall_Gain): bm64_cmd <hex opcode> [params...] -- bt_companion must be ENABLED, log_level debug to see the reply", cmd_bm64_cmd);
#ifdef BOARD_BT_MFB_PIN
    register_cmd("gpiotest", "Is something driving this pin? gpiotest <gpio> (reads with pull-down, then pull-up)", cmd_gpiotest);
    register_cmd("hwcfg", "Hardware config (like Settings -> Hardware): hwcfg [bt none|bm64|bk3266] [lines 1|2] [sd 0|1], then reboot", cmd_hwcfg);
    register_cmd("bm64_pin", "Bit-bang BM64's MFB/RST_N/P2_0 over GPIO18-20 (bench-wired only): bm64_pin <mfb|rst|p20> <0|1> -- single-line, no-microSD boards only", cmd_bm64_pin);
#endif

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

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Our own read loop instead of esp_console_start_repl(): ESP-IDF's goes
     * straight back to linenoise() whenever it returns NULL, and with no
     * computer on the USB port (a charger or battery pack) it returns NULL
     * at once, every time -- measured at ~80% of the CPU, starving the web
     * server, clip playback and Wi-Fi traffic. This one sleeps until a host
     * is there. The REPL object above is still created for its driver/VFS
     * setup; its own task just never gets started (it waits forever on a
     * notification, on the small stack set above). */
    (void)repl;
    xTaskCreate(console_task, "console", 4096, s_prompt, tskIDLE_PRIORITY + 2, NULL);
#else
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
#endif
    ESP_LOGI(TAG, "bring-up console ready — type 'help'");
}
