// ============================================================
// BM64 UART Bridge — ESP32-C6 (AUTO PROGRAMMING MODE)
//
// This sketch auto-enters IBDK/programming mode on every boot,
// then does pure silent transparent passthrough. No serial
// output from the sketch — all bytes flow directly to/from BM64.
//
// This survives the ESP32 DTR-reset that happens when any tool
// (EEPROM_Tool, isupdate.exe) opens the COM port.
//
// To switch between FLASH and NORMAL mode, change BOOT_MODE
// below and re-upload.
//
// Arduino IDE settings:
//   Board:  "ESP32-C6 Dev Module"
//   USB CDC On Boot: "Enabled"
//   Baud:   115200
// ============================================================

// >>> CHANGE THIS TO SWITCH MODES <<<
// 0 = PROGRAMMING/FLASH mode (for EEPROM_Tool / isupdate.exe)
// 1 = NORMAL mode (for SPKCommandSetTool / runtime testing)
#define BOOT_MODE  0

#define BM64_TX_PIN   16  // ESP32-C6 GPIO → BM64 UART_RX
#define BM64_RX_PIN   17  // ESP32-C6 GPIO ← BM64 UART_TX
#define BM64_MFB_PIN  18  // → BM64 MFB (HIGH for normal UART)
#define BM64_RST_PIN  19  // → BM64 RST_N (active LOW)
#define BM64_P20_PIN  20  // → BM64 P2_0 (LOW = programming mode)

#define IBDK_MAGIC 0x1BDC0DE1
RTC_NOINIT_ATTR uint32_t ibdkReady;

uint8_t buf[512];

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(1);  // minimize readBytes blocking

    Serial1.begin(115200, SERIAL_8N1, BM64_RX_PIN, BM64_TX_PIN);
    Serial1.setTimeout(1);

    pinMode(BM64_MFB_PIN, OUTPUT);
    pinMode(BM64_RST_PIN, OUTPUT);
    pinMode(BM64_P20_PIN, OUTPUT);

#if BOOT_MODE == 0
    // --- PROGRAMMING MODE: P2_0 LOW during reset ---
    digitalWrite(BM64_P20_PIN, LOW);
    if (ibdkReady != IBDK_MAGIC) {
        // First boot — full IBDK entry (~800ms)
        ibdkReady = IBDK_MAGIC;
        digitalWrite(BM64_MFB_PIN, LOW);
        digitalWrite(BM64_RST_PIN, LOW);
        delay(100);
        digitalWrite(BM64_RST_PIN, HIGH);
        delay(500);
        digitalWrite(BM64_MFB_PIN, HIGH);
        delay(200);
    } else {
        // DTR reset — BM64 already in IBDK, skip the 800ms
        digitalWrite(BM64_RST_PIN, HIGH);
        digitalWrite(BM64_MFB_PIN, HIGH);
    }
#else
    // --- NORMAL MODE: P2_0 HIGH during reset ---
    digitalWrite(BM64_MFB_PIN, HIGH);
    digitalWrite(BM64_P20_PIN, HIGH);    // P2_0 HIGH = normal application
    digitalWrite(BM64_RST_PIN, LOW);     // Assert reset
    delay(100);
    digitalWrite(BM64_RST_PIN, HIGH);    // Release reset — BM64 boots normally
    delay(500);
#endif

    // Drain any BM64 boot noise before tools connect
    while (Serial1.available()) Serial1.read();
}

void loop() {
    // Pure transparent passthrough — no special char handling.
    // All bytes flow directly between USB CDC and BM64 UART.

    int avail = Serial.available();
    if (avail > 0) {
        int n = Serial.readBytes(buf, min(avail, (int)sizeof(buf)));
        Serial1.write(buf, n);
    }

    avail = Serial1.available();
    if (avail > 0) {
        int n = Serial1.readBytes(buf, min(avail, (int)sizeof(buf)));
        Serial.write(buf, n);
    }
}
