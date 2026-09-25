# BM64 Headphone DAC + Mic Setup via ESP32-C6 UART

## Goal

Bluetooth audio from phone/laptop → BM64 → analog audio out on **A0HPL / A0HPR** pins (headphone DAC),
plus mic input on **MIC_P1** → BM64 → Bluetooth HFP back to phone.
**Mono on both channels. No multi-speaker. No I2S. No line-in.**

---

## Status (2026-09-24)

**EEPROM flashed successfully.** Headphone DAC config confirmed written.

- **Before:** `IS2064S_012_SPK02_V2.1_BETA` (speaker output)
- **After:** `MCHP_DSPKv2.1.7_BM64CLS1.txt` written via isUpdate Tool
- **Verified bytes:** 0x0094=`73`, 0x0171=`46`, 0x01FC=`4A` (all correct)
- **Device Name:** `BM64_DSPK v2.1.5`, BT Address: `40:BC:32:D0:B7:44`

**Next:** pair phone → play audio → probe A0HPL/A0HPR with multimeter for AC signal.

---

## Step 1: Wiring — ESP32-C6 to BM64

**Confirmed 2026-09-23 on the actual "newphone" SMD4 single-line board** (no
microSD populated) — these are the real pins, not placeholders. GPIO16/17
are the board's existing UART header; GPIO18-20 are the free jumper pads
that would otherwise go to line 1's SLIC/microSD (see
`board_pins_smd4.h`'s `BOARD_UART_TX/RX` and `BOARD_BT_MFB/RST/P20_PIN`
macros — this table is just those values written out):

| ESP32-C6 GPIO | → | BM64 Pin | Purpose |
|---------------|---|----------|---------|
| **GPIO16** (TX to BM64) | → | **UART_RX** | ESP32 sends commands to BM64 |
| **GPIO17** (RX from BM64) | → | **UART_TX** | BM64 sends events to ESP32 |
| **GPIO18** | → | **MFB** | Must be HIGH for UART mode -- see note below, this has to be *held* high, a brief pulse isn't enough |
| **GPIO19** | → | **RST_N** | Reset control (active LOW) |
| **GPIO20** | → | **P2_0** | Programming mode (LOW = flash mode) |
| **GND** | → | **GND** | Common ground |

> These are the same GPIOs the project's real ESP-IDF firmware
> (`bt_companion.c`) already drives for normal (non-programming) UART
> bring-up -- confirmed working repeatedly via the `bm64_test`/`bm64_pin`
> console commands before this doc was updated. If you rewire onto a
> different ESP32-C6/board, adjust the `#define`s below to match.

### MFB must be *held* high, not just pulsed
Confirmed the hard way on real hardware: floating/undriven RX showed up as
heavy `0xFF`/`0x00` noise with zero valid frames, and this persisted through
a ground-wire change and a TX/RX swap -- neither was the actual problem.
The fix was holding MFB continuously high (originally tested by tying it
straight to 3.3V, then reproduced purely via GPIO18 held high with a short
~100-200ms settle delay before the first UART command). A brief digitalWrite
pulse that gets released is not enough. GPIO18-20 also don't survive an
ESP32 reset -- they revert to floating inputs -- so re-assert all three
after every reset/reflash, not just once at first boot.

### P2_0 is not related to audio routing
Worth being explicit about, since it's easy to conflate: P2_0 (with EAN,
Table 5-1 in the BM64 datasheet) only selects which application the chip
boots into -- Flash/ROM **Application** (normal run mode, P2_0 high) vs.
**IBDK/Test mode** (P2_0 low during reset, what `isupdate.exe` needs). It
has nothing to do with whether analog audio comes out on A0HPL/A0HPR --
that's purely an EEPROM config bit (see "EEPROM: Which .ipf File to Flash"
below), unrelated to any GPIO pin. P2_0 just gets you into the right *mode*
to go change that EEPROM bit.

### BM64 Power
- BM64 **VBAT** needs 3.3–4.2V (typically from LiPo or 3.3V regulator)
- Do NOT power BM64 from a bare ESP32-C6 dev-kit's own 3.3V pin (insufficient
  current) -- **on this project's actual boards, VBAT comes from the phone
  board's own regulated 3.3V rail** (the same one powering the ESP32 and
  ES8388 codec), which is a real regulator sized for this, not the dev-kit's
  onboard LDO. If you ever bench-test on a bare dev-kit instead of the phone
  board, use an external 3.3V supply, not the dev-kit's 3V3 pin.

---

## Step 2: Arduino Bridge Sketch

The auto-bridge sketch in `bm64_bridge_arduino/` turns the ESP32-C6 into a
transparent USB-to-UART bridge. It auto-enters the correct mode on every boot
(survives the DTR reset that happens when any tool opens the COM port).

**Sketch file:** `bm64_bridge_arduino/bm64_bridge_arduino.ino`

**Arduino IDE settings:**
- Board: **ESP32-C6 Dev Module**
- USB CDC On Boot: **Enabled** (must re-upload after changing this)
- Baud: 115200

**`BOOT_MODE` define** (line 23 of the sketch):
| Value | Mode | Use |
|-------|------|-----|
| `0` | IBDK/Programming | For isUpdate Tool / EEPROM_Tool — auto-enters flash mode on boot |
| `1` | Normal | For runtime testing, controller sketch, SPKCommandSetTool |

Change `BOOT_MODE`, upload, then **fully quit Arduino IDE** before opening
any flash tool. Arduino holds the COM port even when Serial Monitor is closed.

### Gotchas learned the hard way
- **ESP32-C6 DTR reset:** opening/closing the COM port resets the ESP32.
  That's why the sketch auto-enters the mode in `setup()` — interactive
  F/R commands don't survive closing Serial Monitor.
- **Must quit Arduino IDE entirely** (not just close Serial Monitor) before
  isUpdate or EEPROM_Tool can open the port.
- **ESP-IDF `firmware/bm64_bridge/`** has a confirmed bug (VFS layer keeps
  owning USB-CDC input). Don't use it — use the Arduino sketch.
- **`EEPROM_Tool.exe`** can identify the module but fails to write ("cannot
  disconnect the device" error). Use **isUpdate Tool** instead.

---

## Step 3: Flash BM64 EEPROM (confirmed working procedure)

### What you need
- Arduino bridge sketch uploaded with **`BOOT_MODE 0`**
- Arduino IDE **fully quit**
- **isUpdate Tool v3.14** from `DSPK+v2.1.7+Package/Tools/FlashUpdate Tool/`
- EEPROM table: `DSPK+v2.1.7+Package/Software/EEPROM Table/MCHP_DSPKv2.1.7_BM64CLS1.txt`

### Procedure
1. Open **isUpdate Tool** → port: **COM9**, baudrate: **115200** → **Connect**
   - Should show `Port connect -> COM9`, Device: `5506_SPK02`
   - (The ESP32 resets when the tool opens the port, auto-enters IBDK mode)
2. Change **Memory Type → memory** dropdown to **eeprom**
3. In the bottom **Flash/EEPRom/MCU/AHB Access** section:
   - Click **Browse** (the one next to "Write Table")
   - Select `MCHP_DSPKv2.1.7_BM64CLS1.txt`
   - Click **Write Table**
4. **Verify** — still in eeprom mode, use the Read section:
   - Address: `0094`, Length: `01` → should read **`73`**
   - Address: `0171`, Length: `01` → should read **`46`**
   - Address: `01FC`, Length: `01` → should read **`4A`**
5. Disconnect isUpdate Tool

### Verify audio output
1. Change bridge sketch to **`BOOT_MODE 1`** → upload → quit Arduino IDE
2. BM64 boots in normal mode with headphone DAC config
3. Pair phone via Bluetooth (appears as `BM64_DSPK v2.1.5`)
4. Play audio → probe **A0HPL** / **A0HPR** pins with multimeter (AC mV)

### Tool summary

| Tool | Works? | Notes |
|------|--------|-------|
| **isUpdate Tool v3.14** | **Yes** | Use "Write Table" with .txt file, memory type = eeprom |
| EEPROM_Tool.exe | Identifies but fails to write | "cannot disconnect the device" error |
| MPET.exe | Untested | Mass production tool, might also work |

### If isUpdate can't connect
- Make sure **Arduino IDE is fully quit**
- Power cycle the board, try again
- Check Device Manager for correct COM port

---

## Step 4: Query BM64 (normal mode, BOOT_MODE 1)

Use the controller sketch (Step 5) or `SPKCommandSetTool` on PC.

| Command | Hex Bytes | Response |
|---------|-----------|----------|
| Get UART Version | `AA 00 02 08 00 F6` | Event `0x18` — protocol version |
| Read Device Name | `AA 00 02 10 00 EE` | Event `0x21` — BT name |
| Read BT Address | `AA 00 02 0F 00 EF` | Event `0x20` — MAC address |
| Read Link Status | `AA 00 02 0D 00 F1` | Event `0x1E` — connection state |

> **EEPROM bytes can't be read via normal UART protocol.** Use isUpdate Tool
> (memory type = eeprom, Read section) in BOOT_MODE 0 to read specific addresses.

---

## EEPROM Config Reference

### Which file to flash

Located in `DSPK+v2.1.7+Package/Software/EEPROM Table/`. Use the **.txt** file
(not .ipf) with isUpdate Tool's "Write Table" feature.

| File | Output Path | Use |
|------|-------------|-----|
| **`MCHP_DSPKv2.1.7_BM64CLS1.txt`** | **A0HPL / A0HPR (headphone DAC)** | **← FLASHED** |
| `MCHP_DSPKv2.1.7_BM64CLS1_SPKOUT.txt` | SPK_P / SPK_N (Class D amp) | Speaker config |
| `MCHP_DSPKv2.1.7_BM62_*.txt` | — | Wrong IC, don't use for BM64 |

### Critical EEPROM bytes (headphone DAC vs speaker)

| Address | Headphone (CLS1) | Speaker (SPKOUT) | Effect |
|---------|-------------------|---------------------|--------|
| **`0x0094`** | **`0x73`** | `0x13` | Headphone DAC enable on A0HPL/A0HPR |
| **`0x0171`** | **`0x46`** | `0x40` | DAC output path routing |
| **`0x01FC`** | **`0x4A`** | `0x06` | Headphone feature flags |

The .txt files use `**` for "preserve current value" — BT address and other
device-specific bytes are not overwritten.

### Customizing with UI Tool (optional, no hardware needed)

1. Run `UITool_IS206x_012_DualModeSPK_v2.1.32.exe`
2. Load `UITool_..._BM64CLS1_EVB.txt` as starting point
3. Key settings for mono headphone + mic:
   - **Enable LR Sound Mix** — mono on both A0HPL + A0HPR
   - **Voice Stereo** — HFP voice on both channels
   - **Support HF Profile** — mic + voice calls
   - **Support A2DP** — music streaming
4. File → Export → save as new .txt → flash with isUpdate Tool

---

## Step 5: ESP32-C6 Controller Sketch (Runtime)

After EEPROM is flashed, upload this sketch to control the BM64 at runtime:

```cpp
// ============================================================
// BM64 Headphone Audio Controller — ESP32-C6
// BT audio plays on A0HPL/A0HPR, mic input on MIC_P1
// ============================================================

#define BM64_TX_PIN   16  // ESP32-C6 GPIO → BM64 UART_RX
#define BM64_RX_PIN   17  // ESP32-C6 GPIO ← BM64 UART_TX
#define BM64_MFB_PIN  18  // → BM64 MFB
#define BM64_RST_PIN  19  // → BM64 RST_N

// --- Commands ---
const uint8_t CMD_SET_ALL_VOL_MAX[]  = {0xAA, 0x00, 0x07, 0x23, 0x00, 0x00, 0x05, 0x7F, 0x7F, 0x7F, 0x54};
const uint8_t CMD_ENTER_PAIRING[]    = {0xAA, 0x00, 0x03, 0x02, 0x00, 0x5D, 0x9E};
const uint8_t CMD_PLAY_PAUSE[]       = {0xAA, 0x00, 0x03, 0x04, 0x00, 0x05, 0xF4};
const uint8_t CMD_NEXT_TRACK[]       = {0xAA, 0x00, 0x03, 0x04, 0x00, 0x08, 0xF1};
const uint8_t CMD_POWER_ON[]         = {0xAA, 0x00, 0x03, 0x02, 0x00, 0x51, 0xAA};
const uint8_t CMD_GET_UART_VER[]     = {0xAA, 0x00, 0x02, 0x08, 0x00, 0xF6};
const uint8_t CMD_READ_FEATURES[]    = {0xAA, 0x00, 0x02, 0x39, 0x00, 0xC5};
const uint8_t CMD_READ_NAME[]        = {0xAA, 0x00, 0x02, 0x10, 0x00, 0xEE};
const uint8_t CMD_READ_ADDR[]        = {0xAA, 0x00, 0x02, 0x0F, 0x00, 0xEF};
const uint8_t CMD_READ_LINK_STATUS[] = {0xAA, 0x00, 0x02, 0x0D, 0x00, 0xF1};

uint8_t rxBuf[256];
uint8_t rxLen = 0;

uint8_t calcChecksum(const uint8_t* data, uint8_t start, uint8_t end) {
    uint8_t sum = 0;
    for (uint8_t i = start; i <= end; i++) sum += data[i];
    return (~sum) + 1;
}

void sendCmd(const uint8_t* cmd, size_t len) {
    Serial1.write(cmd, len);
    delay(30);
}

void sendAck(uint8_t eventId) {
    uint8_t ack[6] = {0xAA, 0x00, 0x02, 0x14, eventId, 0x00};
    ack[5] = calcChecksum(ack, 2, 4);
    Serial1.write(ack, 6);
}

void handleEvent(uint8_t* data, uint8_t len) {
    uint8_t eventId = data[3];
    Serial.printf("Event 0x%02X [%d bytes]: ", eventId, len);
    for (int i = 0; i < len; i++) Serial.printf("%02X ", data[i]);
    Serial.println();

    // ACK all events except ACK itself
    if (eventId != 0x00) sendAck(eventId);

    // Decode specific events
    switch (eventId) {
        case 0x01: {  // DEVICE_STATE
            uint8_t state = data[4];
            const char* names[] = {"Power OFF","Discoverable","Power ON","","","HFP Connected",
                                   "A2DP Connected","","","SCO (voice call)","","","","","","Standby"};
            if (state <= 0x0F) Serial.printf("  -> State: %s\n", names[state]);
            break;
        }
        case 0x18:  // UART VERSION
            Serial.print("  -> UART Version: ");
            for (int i = 5; i < len - 1; i++) Serial.print((char)data[i]);
            Serial.println();
            break;
        case 0x30:  // BTM_INITIAL_STATUS — BM64 is ready after boot
            Serial.println("  -> BM64 initialized, setting volume to max...");
            sendCmd(CMD_SET_ALL_VOL_MAX, sizeof(CMD_SET_ALL_VOL_MAX));
            break;
    }
}

void printHelp() {
    Serial.println("\n=== BM64 Controller Commands ===");
    Serial.println("  p = Enter pairing mode");
    Serial.println("  SPACE = Play / Pause");
    Serial.println("  n = Next track");
    Serial.println("  v = Set all volumes to max");
    Serial.println("  o = Power ON");
    Serial.println("  i = Query info (UART ver, features, name, address)");
    Serial.println("  s = Read link status");
    Serial.println("  h = Show this help");
    Serial.println("================================\n");
}

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, BM64_RX_PIN, BM64_TX_PIN);

    pinMode(BM64_MFB_PIN, OUTPUT);
    pinMode(BM64_RST_PIN, OUTPUT);

    // Boot sequence: MFB HIGH, then release reset
    digitalWrite(BM64_MFB_PIN, HIGH);
    digitalWrite(BM64_RST_PIN, LOW);
    delay(100);
    digitalWrite(BM64_RST_PIN, HIGH);
    delay(2000);  // BM64 needs ~2s to boot

    printHelp();
}

void loop() {
    // --- Read BM64 events ---
    while (Serial1.available()) {
        uint8_t b = Serial1.read();
        if (rxLen == 0 && b != 0xAA) continue;
        rxBuf[rxLen++] = b;
        if (rxLen >= 4) {
            uint8_t expectedLen = rxBuf[2] + 4;
            if (rxLen >= expectedLen) {
                handleEvent(rxBuf, rxLen);
                rxLen = 0;
            }
        }
        if (rxLen >= sizeof(rxBuf)) rxLen = 0;
    }

    // --- Serial monitor keyboard commands ---
    if (Serial.available()) {
        char c = Serial.read();
        switch (c) {
            case 'p': sendCmd(CMD_ENTER_PAIRING, sizeof(CMD_ENTER_PAIRING));
                      Serial.println(">> Entering pairing mode..."); break;
            case ' ': sendCmd(CMD_PLAY_PAUSE, sizeof(CMD_PLAY_PAUSE));
                      Serial.println(">> Play/Pause"); break;
            case 'n': sendCmd(CMD_NEXT_TRACK, sizeof(CMD_NEXT_TRACK));
                      Serial.println(">> Next track"); break;
            case 'v': sendCmd(CMD_SET_ALL_VOL_MAX, sizeof(CMD_SET_ALL_VOL_MAX));
                      Serial.println(">> Volume max"); break;
            case 'o': sendCmd(CMD_POWER_ON, sizeof(CMD_POWER_ON));
                      Serial.println(">> Power ON"); break;
            case 'i': Serial.println(">> Querying BM64 info...");
                      sendCmd(CMD_GET_UART_VER, sizeof(CMD_GET_UART_VER));
                      delay(100);
                      sendCmd(CMD_READ_FEATURES, sizeof(CMD_READ_FEATURES));
                      delay(100);
                      sendCmd(CMD_READ_NAME, sizeof(CMD_READ_NAME));
                      delay(100);
                      sendCmd(CMD_READ_ADDR, sizeof(CMD_READ_ADDR));
                      break;
            case 's': sendCmd(CMD_READ_LINK_STATUS, sizeof(CMD_READ_LINK_STATUS));
                      Serial.println(">> Read link status"); break;
            case 'h': printHelp(); break;
        }
    }
}
```

### Serial Monitor Keyboard Shortcuts
| Key | Action |
|-----|--------|
| `p` | Enter Bluetooth pairing mode |
| `SPACE` | Play / Pause music |
| `n` | Next track |
| `v` | Set all volumes to maximum |
| `o` | Power ON BM64 |
| `i` | Query device info (version, features, name, BT address) |
| `s` | Read link/connection status |
| `h` | Show help |

---

## UART Protocol Reference

```
AA  00  [LEN]  [CMD_ID]  [PARAMS...]  [CHECKSUM]
```

| Field | Description |
|-------|-------------|
| `AA 00` | Fixed header |
| `LEN` | Byte count of CMD_ID + params |
| `CHECKSUM` | `(~(sum of bytes from LEN through last param) + 1) & 0xFF` |

- **Baud:** 115200, 8N1
- **MFB pin:** Must be HIGH for UART communication

### Checksum Calculation
```c
uint8_t checksum = 0;
for (each byte from LEN to last param)
    checksum += byte;
checksum = ~checksum + 1;  // two's complement
```

---

## UART Commands Reference

### ACK an Event (required — BM64 sends events, ESP32 must ACK them)
```
AA 00 02 14 [EVENT_ID] [CHECKSUM]
```

### Set All Volumes to Max
```
AA 00 07 23 00 00 05 7F 7F 7F 54
```

### Music Control
```
AA 00 03 04 00 05 F4    (play)
AA 00 03 04 00 06 F3    (pause)
AA 00 03 04 00 08 F1    (next track)
```

### Enter Pairing Mode
```
AA 00 03 02 00 5D 9E
```

### Power On / Off
```
AA 00 03 02 00 51 AA    (power on)
AA 00 03 02 00 53 A8    (power off)
```

---

## BM64 Event IDs (sent TO the ESP32)

ACK each with `AA 00 02 14 [EVT_ID] [CHK]`:

| Event ID | Name | Meaning |
|----------|------|---------|
| `0x00` | ACK | Command acknowledgment |
| `0x01` | DEVICE_STATE | Power/pairing/connection state changed |
| `0x02` | CALL_STATUS | Phone call state changed |
| `0x0F` | VOLUME_LEVEL | Volume level report |
| `0x18` | UART_VERSION | Response to GET_UART_VERSION |
| `0x1B` | BTM_UTILITY_REQ | Codec start/stop |
| `0x1E` | REPORT_LINK_STATUS | Current link status |
| `0x2D` | REPORT_TYPE_CODEC | Codec type (SBC/AAC) |
| `0x30` | REPORT_BTM_INITIAL_STATUS | BM64 boot complete |

### Device State Values (event 0x01)
| Value | State |
|-------|-------|
| `0x00` | Power OFF |
| `0x01` | Discoverable (pairing) |
| `0x02` | Power ON (standby) |
| `0x06` | A2DP connected |
| `0x09` | SCO connected (voice call active) |
| `0x0F` | Standby |

---

## File Locations

```
DSPK+v2.1.7+Package/
├── Software/EEPROM Table/
│   ├── MCHP_DSPKv2.1.7_BM64CLS1.txt  ← headphone DAC (FLASHED)
│   └── *.ipf                           ← binary format (EEPROM_Tool only)
├── Tools/
│   ├── FlashUpdate Tool/isupdate.exe   ← USE THIS for EEPROM writes
│   ├── EEPROM_Tool/EEPROM_Tool.exe     ← identifies but can't write via bridge
│   ├── UI Tool/                        ← GUI config editor (no HW needed)
│   └── SpkCommandSetTool/             ← Live UART command GUI
└── Software/Source Code/PIC18 MCU/BM6x_Audio_Refrence.X/
    ├── bt_command_send.c/h            ← UART command builders
    ├── bt_command_decode.c            ← event parser
    └── bt_app.c/h                     ← application state machine
```

---

## IBDK Mode Protocol (for standalone ESP32 writer — WIP)

BM64 in IBDK/programming mode (P2_0 LOW during reset) speaks **HCI** (Host
Controller Interface), not the normal `AA 00` UART protocol. Discovered via
`probe_hci.py` and `sniff_bridge.py`.

**Confirmed working:**
- HCI_Reset: send `01 03 0C 00` → response `04 0E 04 01 03 0C 00`
- `0xFC09` = memory read (reads flash by default, returns data)
- `0xFC0B` = control/config command (short ack response)

**TODO:** discover the command to select memory type (eeprom vs flash) before
read/write. isUpdate Tool does this via its "memory" dropdown. See
`eeprom_write.md` and `probe_hci.py` for details and next steps.

**Update (2026-09-24, DMS log analysis):** Captured and reverse-engineered a
DMS log of isUpdate's actual EEPROM write. Found the real write packet isn't
standard HCI at all — it's a separate raw protocol:
`[04 00 00 00 00 00][122 bytes of EEPROM data]` (header format unconfirmed
beyond "byte 0 = 0x04 for write, block address = 0x0000"). Full write-up in
`eeprom_write.md` under "DMS Log Reverse-Engineering Findings". **Next
step (continue on macOS):** run `test_eeprom_raw_protocol.py` — cross-platform
pyserial script that probes READ-only variants of this raw header format to
confirm the address field encoding, see `eeprom_write.md` "NEXT ACTION"
section for the full file list needed.

---

## Hardware Pins (BM64)

| Pin | Function | Your Use |
|-----|----------|----------|
| **A0HPL** (pin 24) | Headphone DAC Left | Audio output (mono) |
| **A0HPR** (pin 23) | Headphone DAC Right | Audio output (mono) |
| **MIC_P1** (pin 19) | Mic input (+) | Mic input |
| **MIC_N1** (pin 20) | Mic input (−) | Mic ground / bias |
| **UART_TX** | BM64 transmit | → ESP32-C6 RX (GPIO17) |
| **UART_RX** | BM64 receive | ← ESP32-C6 TX (GPIO16) |
| **MFB** | Multi-function button | ← ESP32-C6 GPIO18 (HIGH, held, for UART) |
| **RST_N** | Reset (active low) | ← ESP32-C6 GPIO19 |
| **P2_0** | Programming mode entry | ← ESP32-C6 GPIO20 (LOW during reset = flash mode) |
| **VBAT** | Power supply 3.3–4.2V | External supply (NOT from ESP32 3.3V pin) |
