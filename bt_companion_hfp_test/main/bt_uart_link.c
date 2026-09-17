#include "bt_uart_link.h"
#include "bt_audio_io.h"
#include "bt_led.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "bt_uart_link";

#define LINK_UART       UART_NUM_2
#define LINK_TX_PIN     GPIO_NUM_17   /* ESP32's TX2 -- C6's GPIO17 is its RX */
#define LINK_RX_PIN     GPIO_NUM_16   /* ESP32's RX2 -- C6's GPIO16 is its TX */
#define LINK_BAUD       115200
#define LINK_LINE_MAX        64

static volatile bool s_connected = false;
static esp_bd_addr_t s_peer_addr;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void bt_uart_link_set_connected(bool connected, const esp_bd_addr_t addr)
{
    portENTER_CRITICAL(&s_lock);
    s_connected = connected;
    if (connected && addr) memcpy(s_peer_addr, addr, sizeof(esp_bd_addr_t));
    portEXIT_CRITICAL(&s_lock);
}

static void send_line(const char *line)
{
    uart_write_bytes(LINK_UART, line, strlen(line));
    uart_write_bytes(LINK_UART, "\n", 1);
}

/* Mirrors bt_companion.c's handle_line() on the C6 side -- see that
 * file's own doc comment for the full protocol. Anything unrecognized is
 * logged and ignored, same "don't be fatal over an unknown line" stance
 * the C6 side takes, since this protocol may grow commands either side
 * doesn't know about yet (e.g. RING, once call-routing exists). */
static void handle_line(const char *line)
{
    if (strcmp(line, "STATUS?") == 0) {
        bool connected;
        esp_bd_addr_t addr;
        portENTER_CRITICAL(&s_lock);
        connected = s_connected;
        memcpy(addr, s_peer_addr, sizeof(addr));
        portEXIT_CRITICAL(&s_lock);

        if (connected) {
            char reply[64];
            /* No friendly device name available without a separate GAP
             * remote-name lookup (out of scope for this bring-up pass) --
             * the peer's Bluetooth address is a real, honest identifier
             * in the meantime, just not a pretty one. */
            snprintf(reply, sizeof(reply), "STATUS CONNECTED %02x:%02x:%02x:%02x:%02x:%02x",
                      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            send_line(reply);
        } else {
            send_line("STATUS DISCONNECTED");
        }
    } else if (strcmp(line, "PAIR") == 0) {
        /* Clear any stale bonds first. A phone's own "forget this device"
         * only clears its side -- if this board still has an old link key
         * for that phone's address, the next pairing attempt silently fails
         * (or the phone stops offering the device at all) because the two
         * sides no longer agree on the key. A fresh "Start pairing" click
         * should mean a genuinely fresh attempt. */
        int n = esp_bt_gap_get_bond_device_num();
        if (n > 0) {
            esp_bd_addr_t bonded[n];
            if (esp_bt_gap_get_bond_device_list(&n, bonded) == ESP_OK) {
                for (int i = 0; i < n; i++) {
                    esp_bt_gap_remove_bond_device(bonded[i]);
                }
                ESP_LOGI(TAG, "cleared %d stale bond(s) before pairing", n);
            }
        }
        /* Already connectable/discoverable from boot (main.c) -- this just
         * re-asserts it on request, harmless if already on. */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        bt_led_start_pairing();
        ESP_LOGI(TAG, "PAIR requested -- connectable+discoverable (re)confirmed");
    } else if (strncmp(line, "NAME ", 5) == 0) {
        /* Broadcast the C6 board's own name instead of a fixed string baked
         * into this firmware -- lets one compiled companion image work
         * unmodified across every C6 board (SMD2/SMD4/THT), each with its
         * own name, rather than needing a per-board build just to change
         * what a phone sees while pairing. esp_bt_dev_set_device_name() can
         * be called any time after the stack is up; it takes effect for
         * scans/inquiries from this point on, no reboot needed. */
        {
            char full_name[48];
            snprintf(full_name, sizeof(full_name), "%s-HFP-Test", line + 5);
            esp_bt_dev_set_device_name(full_name);
            ESP_LOGI(TAG, "device name set from C6: '%s'", full_name);
        }
    } else if (strncmp(line, "TONE ", 5) == 0) {
        /* Diagnostic only -- see bt_audio_io_test_tone()'s own doc comment.
         * Bypasses Bluetooth entirely, so this works with no phone call in
         * progress at all, unlike testing with live HFP audio. */
        int hz = 0, ms = 0;
        if (sscanf(line + 5, "%d %d", &hz, &ms) == 2) {
            bt_audio_io_test_tone(hz, ms);
            ESP_LOGI(TAG, "test tone requested: %d Hz for %d ms", hz, ms);
        } else {
            ESP_LOGW(TAG, "bad TONE line from C6: '%s'", line);
        }
    } else if (strncmp(line, "LEVEL ", 6) == 0) {
        /* Diagnostic only -- see bt_audio_io_test_level()'s own doc
         * comment. A constant DC level is much easier to confirm with a
         * multimeter/scope than a moving waveform, useful as a first sanity
         * check before chasing a tone. */
        int high = 0, ms = 0;
        if (sscanf(line + 6, "%d %d", &high, &ms) == 2) {
            bt_audio_io_test_level(high != 0, ms);
            ESP_LOGI(TAG, "test level requested: %s for %d ms", high ? "high" : "low", ms);
        } else {
            ESP_LOGW(TAG, "bad LEVEL line from C6: '%s'", line);
        }
    } else {
        ESP_LOGD(TAG, "unrecognized line from C6: '%s'", line);
    }
}

static void link_task(void *arg)
{
    char line[LINK_LINE_MAX];
    size_t line_len = 0;

    while (1) {
        uint8_t byte;
        int n = uart_read_bytes(LINK_UART, &byte, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) continue;
        if (byte == '\n' || byte == '\r') {
            if (line_len > 0) {
                line[line_len] = '\0';
                handle_line(line);
                line_len = 0;
            }
        } else if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)byte;
        }
    }
}

void bt_uart_link_init(void)
{
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LINK_UART, 256, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(LINK_UART, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART2 init failed: %s -- C6 link won't respond", esp_err_to_name(err));
        return;
    }
    xTaskCreate(link_task, "bt_uart_link", 2560, NULL, tskIDLE_PRIORITY + 1, NULL);
    ESP_LOGI(TAG, "C6 link up on UART2 (GPIO%d=RX, GPIO%d=TX)", LINK_RX_PIN, LINK_TX_PIN);
}
