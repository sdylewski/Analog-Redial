# BK3266 — Cheap BM64 Replacement for Analog Redial

**Updated:** 2026-10-01  
**Status:** Ready to order samples for bench testing

---

## TL;DR

The **BK3266** (Beken, ~$2–3) can likely replace the **BM64** (Microchip,
~$8–12) as the Bluetooth HFP audio bridge, saving **$6–10 per board**.
It has UART call control (answer/reject/hang up), caller ID events,
analog mic input, and analog audio output — everything the project needs
except dial-out to arbitrary numbers.

---

## Why switch from BM64?

| | BM64 | BK3266 |
|--|------|--------|
| Price | $8–12 | **$2–3** |
| BT version | 4.2 | **5.0** |
| DAC SNR | ~90 dB | **98 dB** |
| HFP call control | Full (binary UART) | Full except dial-out |
| Documentation | Excellent (English) | Good (mixed EN/CN) |
| Config tool | EEPROM utility (Win) | ToolKit GUI (Win) |

---

## BK3266 chip specs (from official Beken datasheet)

- **Core:** ARM968E-S @ 48/96 MHz, 256KB ROM, 128KB RAM
- **BT:** 5.0 dual-mode (Classic + BLE)
- **Profiles:** HFP, A2DP, AVRCP, SPP, HID, PBAP
- **Audio:** 16-bit stereo ADC + DAC, 8–48 kHz, SNR 98 dB (DAC) / 96 dB (ADC)
- **DAC output:** 1.1 Vrms diff @ 600R, switchable to single-ended via AT cmd
- **MIC input:** differential or single-ended, switchable via AT cmd
- **Power:** 9.5 mA HFP active, 4 uA standby, 0.4 uA shutdown
- **Supply:** VCC4BAT 2.8–4.2V (battery) or VCC5USB 4.75–5.75V (USB)
- **Package:** QFN32 (4x4mm), TSSOP28, or QFN40 (5x5mm)
- **Temp:** -20 to +80C operating
- **Certifications:** FCC, CE, BQB pre-qualified

---

## Two firmware variants (both use 9600 baud UART)

### 1. Text AT protocol (standard modules)

Simple ASCII commands. Source: [araczkowski/BK3266](https://github.com/araczkowski/BK3266)

**Call control:**

| Command | Function |
|---------|----------|
| `BT+CA\r\n` | Answer call |
| `BT+CJ\r\n` | Reject call |
| `BT+CE\r\n` | Hang up |
| `BT+CR\r\n` | Redial last |
| `BT+PR\r\n` | Enter pairing |
| `BT+AC\r\n` | Reconnect last device |
| `BT+DC\r\n` | Disconnect |

**Config:**

| Command | Function |
|---------|----------|
| `COM+DACSINGLE\r\n` | Single-ended DAC output |
| `COM+MICSINGLE\r\n` | Single-ended MIC input |
| `COM+CALLON\r\n` | Enable HFP (on by default) |
| `COM+SNAME+XXX\r\n` | Set BT device name |
| `COM+Vxx\r\n` | Set volume (00–16) |
| `COM+REBOOT\r\n` | Reboot |

Unknown: whether this firmware sends unsolicited events for caller ID
or call state changes. Needs bench testing.

### 2. Binary framed protocol (BK3266SR modules)

More capable, similar to BM64's protocol. Source: `reference_docs/13618 BK3266SR Commands.xls`

**Frame format:**
```
[0x52 0x42 0x23] [len] [cmd] [data...] [crc] [0x23 0x42 0x52]
  header (3B)     1B    1B    0-NB       1B     footer (3B)

crc = (len + cmd + sum(data)) & 0xFF
```

**MCU → BK3266 commands:**

| CMD | Name | Function |
|-----|------|----------|
| 0x04 | BT_CMD_ACK | **Answer call** |
| 0x05 | BT_CMD_HFHUNG | **Reject call** |
| 0x06 | BT_CMD_NACK | **Hang up** |
| 0x07 | BT_CMD_REDIAL | **Redial last** |
| 0x01 | BT_CMD_MATCH | Enter pairing |
| 0x02 | BT_CMD_CONNECTED | Reconnect |
| 0x03 | BT_CMD_DISCONNECTED | Disconnect |
| 0x08 | BT_CMD_VOLP | Volume + |
| 0x09 | BT_CMD_VOLM | Volume - |
| 0x0A | BT_CMD_CLEAR_MEMORY | Clear pairing list |
| 0x11 | BT_CMD_DEVICE_ADDR | Query BT address |
| 0x12 | BT_CMD_DEVICE_NAME | Query BT name |
| 0x13 | BT_CMD_PHONE_NAME | Query connected phone name |
| 0x16 | BT_CMD_MY | **Query call status** |
| 0x1A | BT_CMD_VOL | Query volume |
| 0x1D | BT_CMD_EESET | Factory reset |
| 0x20 | BT_CMD_MUTE | Mute |
| 0x21 | BT_CMD_UNMUTE | Unmute |
| 0x29 | BT_CMD_BT_NAME | Set BT name |

**BK3266 → MCU unsolicited events:**

| Status | Name | Meaning |
|--------|------|---------|
| 0x64 | APP_CRC_STATUS_ON | Booted |
| 0x65 | APP_CRC_STATUS_II | Link established |
| 0x69 | APP_CRC_STATUS_M1 | **HFP connected** |
| 0x70 | APP_CRC_STATUS_M2 | **Dialing out** |
| 0x71 | APP_CRC_STATUS_M3 | **Outgoing call active** |
| 0x72 | APP_CRC_STATUS_M4 | **In-call** |
| **0x73** | **APP_CRC_STATUS_IC** | **Incoming call (ring)** |
| 0x78 | APP_CRC_STATUS_WC | Pairing successful |

**BK3266 → MCU data events (with payloads):**

| CMD | Name | Payload |
|-----|------|---------|
| **0x03** | **APP_MID_IR** | **Caller's phone number** |
| 0x02 | APP_MID_PR | Outgoing call number |
| 0x04 | APP_MID_VOL | Current volume |
| 0x07 | APP_MID_ADDR | Local BT address |
| 0x08 | APP_MID_NAME | Local BT name |
| 0x09 | APP_MID_PHONENAME | Connected phone name |
| 0x0A | APP_MID_VERSION | Firmware version |

---

## Comparison: BM64 vs BK3266SR for Analog Redial

| Feature | BM64 (current) | BK3266SR (binary) |
|---------|----------------|-------------------|
| Answer call | MMI_ACTION opcode | cmd 0x04 |
| Reject call | MMI_ACTION opcode | cmd 0x05 |
| Hang up | MMI_ACTION opcode | cmd 0x06 |
| Redial last | MMI_ACTION opcode | cmd 0x07 |
| **Dial number** | **MAKE_CALL(digits)** | **NOT AVAILABLE** |
| Caller ID | Event 0x17 (name+number) | APP_MID_IR (number) |
| Call state events | Event 0x01 sub-states | 0x69–0x73 (full lifecycle) |
| Incoming call | Event 0x01 | 0x73 |
| Volume | SET_HF_SPEAKER_GAIN | cmd 0x08/0x09 |
| BT name | EEPROM write | cmd 0x29 |
| MIC mode | EEPROM | `COM+MICSINGLE` |
| DAC mode | EEPROM | `COM+DACSINGLE` |

**The only gap is direct dial-out (no `ATDnumber;` equivalent).**

However, decompiling the ToolKit exe revealed the BK3266 firmware
supports **VOICE NUM 0–9** and **VOICE DIAL** as internal action codes:

| Action code | Name | HFP AT equivalent |
|-------------|------|-------------------|
| 0x0B | HF VOICE DIAL | `AT+BVRA=1` (activate Siri/Google) |
| 0x11 | HF REDIAL | `AT+BLDN` (redial last) |
| **0x13** | **VOICE NUM 0** | **`AT+VTS=0` (DTMF digit 0)** |
| **0x14** | **VOICE NUM 1** | **`AT+VTS=1` (DTMF digit 1)** |
| ... | ... | ... |
| **0x1C** | **VOICE NUM 9** | **`AT+VTS=9` (DTMF digit 9)** |

These are assignable to GPIO buttons via the ToolKit. But the real
question is: **can you send these as UART commands?**

The binary protocol frame `52 42 23 04 01 [action] [crc] 23 42 52`
accepts an action byte. The documented actions go up to 0x12. But the
firmware's action table goes to at least 0x1C. **It's very likely that
sending action 0x13–0x1C over UART will trigger DTMF digits 0–9.**

**Dial-out strategy (needs bench testing):**
1. User rotary-dials a number → ESP32 collects digits
2. ESP32 sends action 0x0B (VOICE DIAL) → activates voice assistant
3. OR: ESP32 initiates a call via companion app over BLE/SPP, then
   sends VOICE NUM actions for any IVR/DTMF menus

This needs to be verified on real hardware. If VOICE NUM actions work
over UART during an active HFP call, you get DTMF over Bluetooth.
Combined with VOICE DIAL (Siri/Google), this provides a workable
dial-out path without needing `ATDnumber;`.

---

## Wiring plan (bench test)

```
ESP32-C6                  BK3266 module
─────────                 ─────────────
UART TX  ───────────────► RX  (pin 11, GPIO1)
UART RX  ◄─────────────── TX  (pin 10, GPIO0)
3.3V     ───────────────► VCC3YS (pin 38)
GND      ───────────────► GND (pin 1/16/25)

                          BK3266 module          SLIC (AG1171)
                          ─────────────          ────────────
                          AUDIOLP (pin 33) ──┐
                              1uF cap        ├──► Line0Vin
                              1K resistor    ┘
                          
                          MICP (pin 29) ◄──── 1uF cap ◄── SLIC Vout
```

Configure after power-up:
```
COM+DACSINGLE    (switch DAC to single-ended)
COM+MICSINGLE    (switch MIC to single-ended)
COM+CALLON       (ensure HFP enabled)
COM+SNAME+newphone  (set BT name)
COM+GOBACKON     (auto-reconnect on boot)
```

---

## What to order

1. **BK3266 module with UART** (~$2–3 on AliExpress) — search for
   "BK3266 BK6988 AT command SPP". Get one with the 10-pin header
   (TX, RX, M+, M- pads visible). Ideally the "SR" variant for the
   binary protocol, but the text AT variant works for initial testing.

2. **BM-3221 module** (~$1.86) — secondary candidate. HFP v1.5, Beken
   chip, but BT 2.1+EDR only and zero documentation. Worth testing
   alongside the BK3266 for comparison.

---

## Chip family reference

| Chip | BT | Status | Notes |
|------|----|--------|-------|
| BK8000L | 2.0+EDR | Obsolete | Original |
| BK6988 / F-6988 | 2.1+EDR | Legacy | Pin-compatible with BK8000L |
| BK3254 | 2.1+EDR | Legacy | Probably inside BM-3221 |
| **BK3266** | **5.0** | **Active** | Current gen, drop-in for all above |

---

## Files in this directory

```
BK3266/
├── BK3266.PDF                    Official Beken datasheet (v0.5, 28pp)
├── BK3266_RESEARCH.md            This file
├── ToolKitV5_8_6_C_20180814/     Beken Windows config tool
│   ├── ToolKitV5_8_6_C_20180814.exe
│   └── ToolKitV5_8_6_C_20180814_mod.exe
└── reference_docs/
    ├── 13618 BK3266SR Commands.xls          Binary protocol command reference
    ├── BK3266SR_command_protocol_V1.0.0.xls  Protocol spec (Chinese)
    ├── BK3266SR_reference_design_V1.0_HYT.pdf  Reference design doc
    ├── SCH_BT_BK3266SR_V3.0.pdf             BK3266SR schematic
    ├── BK3266SR_module_photo.jpg             Module photo
    ├── BK3266_AT_command_reference.rar       AT commands archive (Chinese)
    ├── BK3266_Datasheet_from_github.pdf      Datasheet copy from GitHub
    └── BK_family_footprint_AD.PCBLIB         Altium footprint (BK8002/3254/3260/3266)
```

See also: `../BK-6988/` — F-6988 module datasheet (English, from FCC filing,
confirms BK3266 chip with HFP v1.7)

---

## Sources

- Official Beken datasheet: `BK3266.PDF` (in this directory)
- https://github.com/araczkowski/BK3266 — text AT command list (English)
- https://github.com/bondango/China-Blue-tooth-Audio-Devices — binary
  protocol spreadsheet, schematics, footprint library
- AliExpress listing images (reference schematic, partial AT list, pinout)
- F-6988 FCC filing datasheet (English, in `../BK-6988/`)
