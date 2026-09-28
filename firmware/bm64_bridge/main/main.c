/*
 * bm64_bridge — standalone, do-nothing-else firmware that turns the
 * ESP32-C6 into a transparent USB<->UART bridge to a BM64 companion module,
 * so PC-side tools (isupdate.exe, EEPROM_Tool.exe, SpkCommandSetTool) can
 * talk directly to the BM64's real UART for EEPROM/firmware flashing.
 *
 * This is a SEPARATE idf.py project, not a mode inside the main gateway
 * firmware, on purpose: the main firmware has other tasks (wifi, voip/mDNS,
 * ...) that log to the same USB console at unpredictable times, which would
 * inject stray bytes into the middle of a binary flashing protocol. This
 * image runs nothing else, so there's nothing to interleave.
 *
 * Uses the SAME real esp_console/REPL mechanism this project's bring-up
 * console (bringup.c) uses every day -- confirmed the hard way, across many
 * wrong turns, that RX (host -> device) over native USB Serial/JTAG only
 * ever works from inside a task that has actually gone through the REPL
 * task's own startup (specifically its `setvbuf(stdin, NULL, _IONBF, 0)` --
 * components/console/esp_console_repl.c, esp_console_repl_task()). Every
 * attempt to hand-replicate just the pieces that looked relevant (driver
 * install, vfs use_driver, line-ending modes, fcntl blocking mode, even
 * calling esp_console_new_repl_usb_serial_jtag() itself without also
 * calling esp_console_start_repl()) produced total, silent RX failure --
 * TX (device->host, e.g. boot log) worked every time, RX never did, with
 * no error of any kind. So: this really does start the real REPL. Typing
 * "bridge" at its prompt (once, over a serial terminal) runs the one
 * registered command below, which takes over stdin/stdout permanently for
 * raw forwarding -- from that point on nothing is line-buffered, parsed,
 * or echoed; it's pure binary passthrough until the board is reset.
 *
 * VERIFICATION STATUS (2026-09-23, be aware before trusting this blind):
 * the "bridge" command itself is confirmed dispatching correctly against
 * real hardware (typed over a real serial connection, got the expected
 * printed response back). What is NOT independently confirmed from this
 * Mac: a full round trip through the raw forwarding loop to BM64 and back.
 * Every attempt to verify that specific piece tonight, across several
 * different scripted test approaches (including a full close+reopen of the
 * serial port between steps), produced inconsistent/corrupted results --
 * bytes arriving out of order or missing entirely, in ways that don't point
 * at this code so much as at this Mac's native-USB-CDC host state after a
 * very long session of rapid repeated flash/reset cycles. First thing to
 * try at a fresh machine: a real interactive terminal (PuTTY, TeraTerm,
 * screen) rather than a scripted one-shot connection, after a full physical
 * USB unplug/replug for a clean enumeration.
 *
 * Flash this to the SMD4 board temporarily (idf.py -p <port> flash), open
 * a real serial terminal (PuTTY, screen, TeraTerm -- NOT a one-shot
 * scripted send), type "bridge", then point the PC-side tool at that same
 * port. Reflash the real gateway firmware afterward -- see
 * Bluetooth/BM64_Analog_Audio_Setup.md.
 *
 * Pins match board_pins_smd4.h / BOARD_UART_TX/RX and BOARD_BT_MFB/RST/
 * P20_PIN exactly (confirmed on real "newphone" hardware 2026-09-23) --
 * hardcoded here rather than pulling in the `board` component, since this
 * project intentionally has no other dependency on the main firmware tree.
 */
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <unistd.h>

#define BM64_TX_PIN   GPIO_NUM_16   /* ESP32 -> BM64 UART_RX */
#define BM64_RX_PIN   GPIO_NUM_17   /* ESP32 <- BM64 UART_TX */
#define BM64_MFB_PIN  GPIO_NUM_18
#define BM64_RST_PIN  GPIO_NUM_19
#define BM64_P20_PIN  GPIO_NUM_20
#define BM64_UART     UART_NUM_1
#define BM64_BAUD     115200

static void bm64_uart_init(void)
{
    gpio_set_direction(BM64_MFB_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BM64_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BM64_P20_PIN, GPIO_MODE_OUTPUT);

    uart_config_t cfg = {
        .baud_rate  = BM64_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(BM64_UART, 1024, 1024, 0, NULL, 0);
    uart_param_config(BM64_UART, &cfg);
    uart_set_pin(BM64_UART, BM64_TX_PIN, BM64_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/* P2_0 low selects IBDK/Test mode (Table 5-1, BM64 datasheet) instead of
 * the normal Flash Application; a reset pulse latches it (mode-select pins
 * are sampled only at reset, not continuously -- setting them without a
 * real low-to-high RST_N edge does nothing if a prior boot already latched
 * a different mode). EAN isn't separately driven -- its internal pull-high
 * (Table 5-1 note 1) combined with P2_0 low lands on "Low High High = ROM
 * IBDK", one of the two valid IBDK rows. */
static void enter_programming_mode(void)
{
    gpio_set_level(BM64_P20_PIN, 0);
    gpio_set_level(BM64_MFB_PIN, 0);
    gpio_set_level(BM64_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BM64_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(BM64_MFB_PIN, 1);
}

/* BM64 -> USB, in its own task so the raw forwarding below can block on
 * read() without starving this direction. */
static void bm64_to_usb_task(void *arg)
{
    uint8_t buf[256];
    while (1) {
        int m = uart_read_bytes(BM64_UART, buf, sizeof(buf), portMAX_DELAY);
        if (m > 0) write(STDOUT_FILENO, buf, (size_t)m);
    }
}

/* The one registered console command. Runs in the REPL task itself, which
 * is exactly why RX works here (see the top-of-file comment) -- never
 * returns, so the REPL's own line-parsing loop never gets control back;
 * from here on stdin/stdout carry pure binary BM64 protocol, not console
 * text. */
static int cmd_bridge(int argc, char **argv)
{
    printf("Entering raw BM64 bridge mode -- BM64 held in IBDK/programming "
           "mode now. No further console output past this line; reset the "
           "board to get the console prompt back.\r\n");
    fflush(stdout);
    clearerr(stdin); /* defensive: a stuck EOF/error flag on stdin would make every read below return EOF forever */

    enter_programming_mode();
    xTaskCreate(bm64_to_usb_task, "bm64_to_usb", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);

    /* One byte at a time via fgetc(), not a buffered read() -- matches the
     * one mechanism (linenoise's own character-at-a-time reads) confirmed
     * to actually receive bytes on this hardware; a buffered read() was
     * tried first and, in every test, received nothing past the initial
     * command line. Slower, but 115200 baud has no shortage of headroom
     * for this. */
    while (1) {
        int c = fgetc(stdin);
        if (c != EOF) {
            uint8_t b = (uint8_t)c;
            uart_write_bytes(BM64_UART, (const char *)&b, 1);
        }
    }
    return 0; /* unreached */
}

void app_main(void)
{
    bm64_uart_init();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "bm64_bridge>";
    esp_console_dev_usb_serial_jtag_config_t usb_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_new_repl_usb_serial_jtag(&usb_cfg, &repl_cfg, &repl);

    esp_console_cmd_t cmd = {
        .command = "bridge",
        .help = "Enter raw BM64 UART bridge mode (does not return; reset to leave)",
        .func = cmd_bridge,
    };
    esp_console_cmd_register(&cmd);

    esp_console_start_repl(repl);
}
