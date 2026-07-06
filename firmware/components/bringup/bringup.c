#include "bringup.h"
#include "board.h"
#include "board_pins.h"
#include "wm8731.h"
#include "audio.h"
#include "ag1170.h"
#include "dtmf.h"
#include "telephony.h"
#include "esp_console.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "bringup";

/* ---------- i2c_scan ---------- */
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
                   addr == BOARD_WM8731_ADDR ? "  <- WM8731" : "");
            found++;
        }
    }
    printf("%d device(s) found%s\n", found,
           found == 0 ? " (expected if no PCB / codec not powered)" : "");
    return 0;
}

/* ---------- wm8731_init / dump ---------- */
static int cmd_wm8731_init(int argc, char **argv)
{
    wm8731_config_t c = WM8731_DEFAULT_CONFIG();
    c.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    esp_err_t err = wm8731_init(&c);
    printf("wm8731_init: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_wm8731_dump(int argc, char **argv)
{
    const char *names[] = {
        "LLINEIN", "RLINEIN", "LHPOUT", "RHPOUT", "ANALOG", "DIGITAL",
        "POWER", "IFACE", "SAMPLING", "ACTIVE"
    };
    printf("WM8731 shadow registers (write-only device):\n");
    for (int i = 0; i < 10; i++) {
        printf("  R%-2d %-9s = 0x%03X\n", i, names[i], wm8731_shadow(i));
    }
    return 0;
}

static int cmd_wm8731_bypass(int argc, char **argv)
{
    bool on = (argc > 1 && atoi(argv[1]) != 0);
    printf("analog bypass %s: %s\n", on ? "ON" : "OFF",
           esp_err_to_name(wm8731_set_bypass(on)));
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
    int steps = ms / 200;
    for (int i = 0; i < steps; i++) {
        printf("  L0 rms=%5u   L1 rms=%5u\n",
               audio_rx_level(0), audio_rx_level(1));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
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

static void register_cmd(const char *cmd, const char *help, esp_console_cmd_func_t fn)
{
    esp_console_cmd_t c = { .command = cmd, .help = help, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void bringup_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "phone>";
    repl_cfg.max_cmdline_length = 128;

    esp_console_register_help_command();

    register_cmd("i2c_scan", "Scan I2C bus for the WM8731", cmd_i2c_scan);
    register_cmd("wm8731_init", "Initialize the codec", cmd_wm8731_init);
    register_cmd("wm8731_dump", "Dump codec shadow registers", cmd_wm8731_dump);
    register_cmd("wm8731_bypass", "Analog bypass on/off: wm8731_bypass <0|1>", cmd_wm8731_bypass);

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
    }

    register_cmd("dialtone", "Dial tone: dialtone [line]", cmd_progress);
    register_cmd("ringback", "Ringback tone: ringback [line]", cmd_progress);
    register_cmd("busy", "Busy tone: busy [line]", cmd_progress);
    register_cmd("silence", "Stop audio: silence [line]", cmd_silence);
    register_cmd("loopback", "Mic->speaker: loopback [line] [0|1]", cmd_loopback);
    register_cmd("capture", "RX level meter: capture [ms]", cmd_capture);
    register_cmd("dtmf_gen", "Emit DTMF: dtmf_gen <digits> [line]", cmd_dtmf_gen);
    register_cmd("slic", "SLIC pin: slic <line> <pd|fr|rm> <0|1>", cmd_slic);
    register_cmd("ring", "Ring a line: ring [line] [secs]", cmd_ring);
    register_cmd("hookmon", "Watch SHK: hookmon [secs]", cmd_hookmon);
    register_cmd("status", "Per-line telephony status", cmd_status);

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "bring-up console ready — type 'help'");
}
