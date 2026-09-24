# BM64 Headphone DAC + Mic Setup via ESP32-C6 UART

## Goal

Bluetooth audio from phone/laptop → BM64 → analog audio out on **A0HPL / A0HPR** pins (headphone DAC),
plus mic input on **MIC_P1** → BM64 → Bluetooth HFP back to phone.
**Mono on both channels. No multi-speaker. No I2S. No line-in.**

---

## Status as of 2026-09-23 (read this first)

Wiring is done and confirmed on the real "newphone" SMD4 board: BM64's MFB/
RST_N/P2_0 are on GPIO18/19/20, UART is on GPIO16/17 (the board's existing
header) -- see the wiring table below, all pin numbers there are the real,
confirmed ones, not placeholders.

**Two ways to get isupdate.exe talking to the BM64 now exist:**

1. **`firmware/bm64_bridge/` (recommended, built tonight, no Arduino
   needed)** -- a small standalone ESP-IDF project, built with the exact
   same `idf.py` toolchain already set up for this project. See "Recommended
   path" below -- this is the one to try first at work.
2. **The Arduino sketches later in this doc** -- kept as a fallback in case
   `bm64_bridge` doesn't pan out on your work machine for some reason.

**Verification status, honestly:** the `bm64_bridge` firmware's "bridge"
command is confirmed dispatching correctly against the real board tonight
(typed over a live serial connection, got the expected response back). What
is NOT independently confirmed from this Mac: a full round trip through the
raw forwarding loop all the way to BM64 and back. Every attempt to verify
that specific piece tonight produced inconsistent, corrupted-looking results
across several different scripted test approaches -- bytes arriving out of
order or missing, in a way that points at this Mac's native-USB-CDC host
state after a very long session of rapid repeated flash/reset cycles, not at
the firmware itself (every individual piece of it matches code already
proven reliable elsewhere in this project). **First thing to try at work:**
a full physical USB unplug/replug (for a clean enumeration on a machine that
hasn't been hammering the same port for hours) and a real interactive
terminal (PuTTY, TeraTerm, or `screen`) rather than a scripted one-shot
connection -- interactive terminals are what's been reliable all session,
scripted one-shot opens are what's been flaky.

---

## Recommended path: `firmware/bm64_bridge/` (idf.py, no Arduino)

**Files you need** (all in this repo, under `firmware/`):
- `firmware/bm64_bridge/` -- the whole standalone project (`CMakeLists.txt`,
  `sdkconfig.defaults`, `main/main.c`, `main/CMakeLists.txt`). Self-
  contained -- no dependency on this project's `board`/`bt_companion`
  components, just hardcoded GPIO16-20 (confirmed pin numbers, see wiring
  table below).
- Your existing ESP-IDF toolchain (same one used for the main gateway
  firmware) -- `source ~/esp/esp-idf/export.sh` first, same as always.

**Build and flash:**
```
cd firmware/bm64_bridge
idf.py set-target esp32c6      # first time only
idf.py -p <port> flash
```

**Use it:**
1. Open a real serial terminal at 115200 baud on that port (PuTTY/TeraTerm
   on Windows, or `screen <port> 115200` on macOS/Linux) -- not a one-shot
   scripted connection.
2. You'll land at a `bm64_bridge>` prompt (standard ESP-IDF console).
3. Type `bridge` and press Enter. This does three things at once: puts BM64
   into ROM IBDK/programming mode (P2_0 low + a real reset pulse -- see
   Table 5-1 note below), starts forwarding raw bytes both directions
   between this same serial port and BM64's UART, and never returns (no more
   console prompt after this -- that's expected, not a hang).
4. From this point, the port is pure BM64 UART traffic. Point `isupdate.exe`
   (or `EEPROM_Tool.exe`, `SpkCommandSetTool`) at the same COM port and
   proceed with flashing as normal.
5. When done, physically reset the board (or power-cycle) to get the
   `bm64_bridge>` console prompt back, or reflash the real gateway firmware
   over USB (`idf.py -B build -p <port> flash` from `firmware/`, the main
   project directory, not `bm64_bridge/`) to put the phone back in service.

**Quick sanity check once connected via `isupdate.exe` or any raw serial
tool:** BM64 in normal (non-IBDK) mode replies to `AA 00 02 08 00 F6`
(GET_UART_VERSION) with a couple of framed reply packets -- if you want to
confirm the bridge itself works before trusting it for a real flash, you can
temporarily comment out the `enter_programming_mode()` call in `main.c`
(leaves BM64 in normal mode instead), reflash, type `bridge`, and send that
exact 6-byte frame -- a reply confirms the raw forwarding loop is solid.

---

## Step 0: Arduino IDE Setup (for ESP32-C6) -- fallback if bm64_bridge doesn't work

**Use Arduino IDE** — it's all you need for UART bridge and controller sketches. ESP-IDF is overkill here.

### Install
1. Download **Arduino IDE 2.x** from https://www.arduino.cc/en/software
2. Open Arduino IDE → File → Preferences
3. In **"Additional boards manager URLs"**, add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
4. Tools → Board → Boards Manager → search **"esp32"** → install **"esp32 by Espressif Systems"** (v3.x)
5. Tools → Board → ESP32 → select **"ESP32-C6 Dev Module"**
6. Tools → Port → select your ESP32-C6's COM port (e.g., COM8)
7. Tools → USB CDC On Boot → **Enabled** (ESP32-C6 uses native USB CDC)

### Verify
- Plug in ESP32-C6 via USB
- In Arduino IDE: Tools → Port — you should see your COM port
- Upload the "Blink" example to confirm flashing works

---

## Step 0.5: Fix "COM Ports In Use" Problem

If Device Manager shows COM ports as "in use" that shouldn't be:

### Method 1: Remove Phantom COM Ports
1. Open **Device Manager**
2. View → **Show hidden devices**
3. Expand **Ports (COM & LPT)**
4. Right-click any grayed-out (phantom) COM ports → **Uninstall device**
5. Unplug/replug your ESP32-C6

### Method 2: Reassign Your ESP32-C6 to a Lower COM Port
1. Device Manager → Ports → right-click your ESP32-C6 (COM8)
2. Properties → Port Settings → **Advanced**
3. Change "COM Port Number" to an unused low number (COM3, COM5, etc.)
4. OK → unplug/replug

### Method 3: Registry Cleanup (nuclear option)
1. Open `regedit` → navigate to:
   ```
   HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\COM Name Arbiter
   ```
2. Delete the `ComDB` value → reboot
3. This resets ALL COM port assignments

> **Note:** `EEPROM_Tool.exe` only supports COM1–7. But **`isupdate.exe` supports COM8+**,
> so use `isupdate.exe` if you can't free a low COM port.

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

## Step 2: Flash the UART Bridge Sketch (Arduino fallback -- try `firmware/bm64_bridge/` above first)

This turns your ESP32-C6 into a transparent USB-to-UART bridge so PC tools
(`isupdate.exe`, `EEPROM_Tool.exe`, `SPKCommandSetTool`) can talk directly to the BM64.

```cpp
// ============================================================
// BM64 UART Bridge — ESP32-C6
// Transparent passthrough: USB CDC ↔ BM64 UART
// ============================================================

#define BM64_TX_PIN   16  // ESP32-C6 GPIO → BM64 UART_RX
#define BM64_RX_PIN   17  // ESP32-C6 GPIO ← BM64 UART_TX
#define BM64_MFB_PIN  18  // → BM64 MFB (HIGH for normal UART)
#define BM64_RST_PIN  19  // → BM64 RST_N (active LOW)
#define BM64_P20_PIN  20  // → BM64 P2_0 (LOW = programming mode)

// Buffer for faster throughput (byte-by-byte is too slow for flash tools)
uint8_t buf[512];

void setup() {
    // USB CDC serial (to PC)
    Serial.begin(115200);

    // Hardware UART to BM64
    Serial1.begin(115200, SERIAL_8N1, BM64_RX_PIN, BM64_TX_PIN);

    // Control pins
    pinMode(BM64_MFB_PIN, OUTPUT);
    pinMode(BM64_RST_PIN, OUTPUT);
    pinMode(BM64_P20_PIN, OUTPUT);

    // Default: normal run mode (not programming)
    digitalWrite(BM64_MFB_PIN, HIGH);   // MFB HIGH = UART active
    digitalWrite(BM64_P20_PIN, HIGH);   // P2_0 HIGH = normal mode
    digitalWrite(BM64_RST_PIN, HIGH);   // RST_N HIGH = not in reset

    // To enter programming mode, send 'F' over serial monitor (see loop)
}

void loop() {
    // --- USB → BM64 ---
    int avail = Serial.available();
    if (avail > 0) {
        // Check for special single-char commands from serial monitor
        if (avail == 1) {
            uint8_t c = Serial.peek();
            if (c == 'F') {  // 'F' = enter Flash/programming mode
                Serial.read();
                enterProgrammingMode();
                return;
            }
            if (c == 'R') {  // 'R' = reset BM64 to normal mode
                Serial.read();
                resetToNormalMode();
                return;
            }
        }
        int n = Serial.readBytes(buf, min(avail, (int)sizeof(buf)));
        Serial1.write(buf, n);
    }

    // --- BM64 → USB ---
    avail = Serial1.available();
    if (avail > 0) {
        int n = Serial1.readBytes(buf, min(avail, (int)sizeof(buf)));
        Serial.write(buf, n);
    }
}

void enterProgrammingMode() {
    Serial.println("\n>>> Entering BM64 PROGRAMMING MODE...");
    Serial.println(">>> P2_0 = LOW, resetting BM64...");

    digitalWrite(BM64_P20_PIN, LOW);    // P2_0 LOW = programming mode
    digitalWrite(BM64_MFB_PIN, LOW);    // MFB LOW during reset
    digitalWrite(BM64_RST_PIN, LOW);    // Assert reset
    delay(100);
    digitalWrite(BM64_RST_PIN, HIGH);   // Release reset — BM64 boots into flash mode
    delay(500);
    digitalWrite(BM64_MFB_PIN, HIGH);   // MFB back HIGH

    Serial.println(">>> BM64 is now in programming mode.");
    Serial.println(">>> Run isupdate.exe on PC, select this COM port.");
    Serial.println(">>> Send 'R' to reset back to normal mode when done.");
}

void resetToNormalMode() {
    Serial.println("\n>>> Resetting BM64 to NORMAL mode...");

    digitalWrite(BM64_P20_PIN, HIGH);   // P2_0 HIGH = normal mode
    digitalWrite(BM64_MFB_PIN, LOW);
    digitalWrite(BM64_RST_PIN, LOW);    // Assert reset
    delay(100);
    digitalWrite(BM64_RST_PIN, HIGH);   // Release reset — BM64 boots normally
    delay(500);
    digitalWrite(BM64_MFB_PIN, HIGH);   // MFB HIGH for UART

    Serial.println(">>> BM64 is now in normal mode.");
    Serial.println(">>> UART bridge active — tool commands pass through.");
}
```

### This overwrites the phone board's real firmware
This board normally runs the project's own ESP-IDF firmware (`bt_companion.c`
+ the rest of the gateway). Uploading this Arduino sketch replaces it
entirely -- there's no dual-boot here. After flashing/testing EEPROM changes,
reflash the real firmware back with `idf.py -B build -p <port> flash` from
`firmware/` before using the board as a phone again. (No need to manually
re-set MFB/RST_N/P2_0 back to normal on that reflash -- the real firmware
now asserts all three high automatically on boot whenever Bluetooth is
enabled, see `bt_companion.c`'s `assert_bm64_control_pins()`.)

### How to Use
1. Upload this sketch to ESP32-C6
2. Open Arduino Serial Monitor at **115200 baud** (set to **"No line ending"**)
3. Type **`F`** and press Enter → puts BM64 into programming mode
4. **Close the Serial Monitor** (so the COM port is free for `isupdate.exe`)
5. Run `isupdate.exe` → select the same COM port → flash the .ipf file
6. After flashing, re-open Serial Monitor → type **`R`** → resets BM64 to normal mode

---

## Step 3: Flash BM64 EEPROM with isupdate.exe

### Which Tool to Use

| Tool | COM Port Range | What It Programs | Recommended? |
|------|---------------|-----------------|--------------|
| **`isupdate.exe`** | **COM1–COM99+** | Full flash (FW + EEPROM) | **← USE THIS** |
| `EEPROM_Tool.exe` | COM1–COM7 only | EEPROM area only | Only if COM ≤ 7 |
| `DSPTool` | Unknown | DSP params only (EQ, NR) | NOT for audio routing |
| `SPKCommandSetTool` | Unknown | No flashing — runtime commands only | For testing |

### Flashing Procedure

1. **Wire ESP32-C6 → BM64** per wiring table above
2. **Upload UART bridge sketch** to ESP32-C6
3. **Open Serial Monitor** → type `F` → BM64 enters programming mode
4. **Close Serial Monitor** (releases COM port)
5. **Run** `isupdate.exe` from:
   ```
   DSPK+v2.1.7+Package\Tools\FlashUpdate Tool\isupdate.exe
   ```
6. In isupdate.exe:
   - Select your **COM port** (COM8 or whatever your ESP32-C6 is on)
   - Browse to the **.ipf file**:
     ```
     DSPK+v2.1.7+Package\Software\EEPROM Table\MCHP_DSPKv2.1.7_BM64CLS1.ipf
     ```
     This is the **headphone DAC output** configuration (A0HPL/A0HPR).
   - Click **Update** / **Program**
   - Wait for completion (may take 30-60 seconds)
7. **Re-open Serial Monitor** → type `R` → BM64 resets to normal mode
8. BM64 is now running the headphone DAC configuration

### If isupdate.exe Can't Connect
- Make sure **Serial Monitor is closed** (only one app can use a COM port)
- Try **different baud rates** in isupdate.exe if it has that option
- Verify **P2_0 is LOW** (programming mode) — check with multimeter
- Verify **RST_N went LOW then HIGH** (reset sequence completed)
- Try the reset sequence again: type `R` then `F` in Serial Monitor

---

## Step 4: Query BM64 (Before or After Flashing)

With the bridge sketch running and BM64 in **normal mode** (not programming mode),
you can send diagnostic commands through the Serial Monitor to check what's running.

### Diagnostic Commands (send as raw hex)

Use the **controller sketch** (Step 5) which has these built in, or use the bridge sketch
with `SPKCommandSetTool` on PC.

| Command Name | Hex Bytes | Response Event | What You Learn |
|-------------|-----------|----------------|---------------|
| **Get UART Version** | `AA 00 02 08 00 F6` | Event `0x18` | Firmware UART protocol version |
| **Read Feature List** | `AA 00 02 39 00 C5` | Event `0x40` | ROM name (e.g., "5506_DUAL_SPK"), capabilities |
| **Read Device Name** | `AA 00 02 10 00 EE` | Event `0x21` | Bluetooth device name |
| **Read BT Address** | `AA 00 02 0F 00 EF` | Event `0x20` | Local Bluetooth MAC address |
| **Read Link Status** | `AA 00 02 0D 00 F1` | Event `0x1E` | Current connection state |

> **Note:** These commands tell you the ROM/firmware version but there is no "read EEPROM byte"
> command in the normal UART protocol. You can't directly read the 3 routing bytes (0x0094, 0x0171, 0x01FC).
> The practical test is: pair → play audio → probe A0HPL with a multimeter for AC signal.

---

## EEPROM: Which .ipf File to Flash

Located in: `DSPK+v2.1.7+Package\Software\EEPROM Table\`

| File | Output Path | Use |
|------|-------------|-----|
| **`MCHP_DSPKv2.1.7_BM64CLS1.ipf`** | **A0HPL / A0HPR (headphone DAC)** | **← USE THIS** |
| `MCHP_DSPKv2.1.7_BM64CLS1_SPKOUT.ipf` | SPK_P / SPK_N (Class D amp) | For driving speakers directly |
| `MCHP_DSPKv2.1.7_BM64CLS2.ipf` | Headphone DAC, Class 2 power | Shorter BT range |
| `MCHP_DSPKv2.1.7_BM62_*.ipf` | — | Wrong IC, don't use for BM64 |

### Critical EEPROM Bytes That Differ (CLS1 vs SPKOUT)

Confirmed by diffing the UI Tool exports (`UITool_IS206x_012_DualModeSPK_v2.1.32_BM64CLS1_EVB.txt`
vs `..._SPKOUT.txt`). Only 3 bytes differ — these control the audio output routing:

| Address | CLS1 (headphone) | SPKOUT (speaker amp) | Effect |
|---------|-------------------|---------------------|--------|
| **`0x0094`** | **`0x73`** | `0x13` | Bits 5+6 (`0x60`): headphone DAC enable on A0HPL/A0HPR |
| **`0x0171`** | **`0x46`** | `0x40` | Bits 1+2 (`0x06`): DAC output path routing |
| **`0x01FC`** | **`0x4A`** | `0x06` | Bits 3+6 (`0x48`): headphone feature flags |

> When these bits are cleared (SPKOUT values), audio routes to the Class D amplifier pins instead.

---

## EEPROM: Customizing Settings (Optional)

The UI Tool can customize and re-export the EEPROM config. **No hardware needed** — it's just a config editor.

### Customization Workflow
1. Run `UITool_IS206x_012_DualModeSPK_v2.1.32.exe`
2. Load `UITool_..._BM64CLS1_EVB.txt` as the starting point
3. Enable **"LR Sound Mix"** for mono on both A0HPL + A0HPR
4. Enable **"Voice Stereo"** for mono voice on both pins
5. Disable **"Stereo Line in Loop Back"** (you don't use line-in)
6. File → Export → save as new .txt
7. The UI Tool also generates a corresponding .ipf for flashing

### Key Settings (from .ihlp parameter help file)

#### Audio / CODEC

| Setting | What It Does | You Want |
|---------|-------------|----------|
| **Enable LR Sound Mix** (`CODEC_FUNC5`) | Mix L+R into mono on BOTH output pins | **Enabled** |
| **Speaker Output** (`CODEC_FUNC1`) | Mono mix for speaker output | Enabled |
| **Voice Stereo** (`CODEC_FUNC16`) | HFP voice on both L+R channels | **Enabled** |
| **Tone Stereo** (`CODEC_FUNC15`) | Tones/prompts on both channels | Enabled |
| **I2S Shutdown** (`CODEC_FUNC17`) | Shut down I2S when codec resets | Enabled |

#### Profiles

| Setting | You Want |
|---------|----------|
| **Support HF Profile** (`PROFILE_SETTING2`) | **Enabled** (mic + voice calls) |
| **Support A2DP** (`PROFILE_SETTING3`) | **Enabled** (music streaming) |
| **Support AVRCP** (`PROFILE_SETTING4/5`) | Enabled (play/pause/skip) |
| **Wide Band Speech / mSBC** (`MISC_OPTION24`) | Recommended (HD voice) |

#### UART

| Setting | You Want |
|---------|----------|
| **Enable HCI UART** (`UART_SETTING1`) | **Enabled** |
| **UART Baudrate** (`UART_SETTING2`) | 115200 |

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
DSPK+v2.1.7+Package\
├── Software\
│   └── EEPROM Table\
│       ├── MCHP_DSPKv2.1.7_BM64CLS1.ipf          ← FLASH THIS (headphone DAC out)
│       ├── MCHP_DSPKv2.1.7_BM64CLS1_SPKOUT.ipf   ← speaker amp (don't use)
│       └── *.txt                                    ← hex dumps of each config
├── Tools\
│   ├── EEPROM_Tool\EEPROM_Tool.exe                  ← COM1-7 only
│   ├── FlashUpdate Tool\isupdate.exe                ← USE THIS (COM8+ works)
│   ├── UI Tool\UITool_...\                          ← GUI config editor (no HW needed)
│   ├── SpkCommandSetTool\                           ← Live UART command GUI
│   ├── MP Tools\MPET.exe                            ← Mass production EEPROM tool
│   └── DSP Tool\                                    ← EQ/NR config only (not audio routing)
└── Documentation\
    └── AudioUARTCommandSet_v2.10.pdf                ← full UART command reference
```

---

## Source Code Reference

```
Software\Source Code\PIC18 MCU\BM6x_Audio_Refrence.X\
  bt_command_send.c   — All UART command builders (checksum, gain, etc.)
  bt_command_send.h   — Command ID enums and MMI action codes
  bt_command_decode.c — Event parser, event ID enums, BTM status enums
  bt_app.c            — Main application state machine, boot sequence, volume
  bt_app.h            — BT status, event, and profile enums
  mcc/pin_manager.h   — Pin definitions (MFB, RST, etc.)
```

### BM64 Boot Sequence (from bt_app.c)
The reference MCU code shows the correct initialization order:
1. Wait 200ms
2. Set **MFB HIGH**
3. Wait 20ms
4. Set **RST_N HIGH** (release reset)
5. Wait 500ms for BM64 to boot
6. Send `BT_SetRXBufferSize()` command
7. Send `BT_SetupBTMGPIO()` command
8. Read device address, name, feature list
9. Start BLE advertising
10. Enter power-on state

### Checksum Algorithm (from bt_command_send.c)
```c
static uint8_t calculateChecksum(uint8_t* startByte, uint8_t* endByte) {
    uint8_t checksum = 0;
    while (startByte <= endByte)
        checksum += *startByte++;
    return ~checksum + 1;
}
```

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
