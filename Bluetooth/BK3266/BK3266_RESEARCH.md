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

## Pinout reference (BK3266SR module, 40-pin)

Read directly off the official pin description table in
`reference_docs/BK3266SR_module_manual_ENGLISH.pdf` (§ IO description) and
cross-checked against the reference schematic
(`reference_docs/SCH_BT_BK3266SR_V3.0.pdf`) — not inferred from the Chinese
silkscreen names alone, which differ from the datasheet's own names in a
couple of places (the schematic calls pins 27/28 `LINE_L`/`LINE_R`; the
datasheet calls the same pins `AUX_R`/`AUX_L`). Only the pins that matter
for this project; the full 40-pin table is in the manual.

**No reset pin exists on this module at all** — unlike the BM64, which has
an `RST_N` you choose to leave unconnected, BK3266SR's pinout simply has no
reset signal to route. Same for BM64's `EAN`/`P2_0` programming-mode
strap — there's no equivalent here either.

| Pin | Name | What it does |
|-----|------|---------------|
| 37 | **VBAT** | **Power input, 3.3–4.2 V.** The one real power-in pin — feed the board's +3.3V rail here. *(Pins 38/39 below are chip-internal 3V **outputs**, not inputs — don't confuse them with VBAT.)* |
| 1, 3, 16, 25 | GND | Digital/power ground |
| 40 | AGND | Separate analog ground. Datasheet: for single-ended audio output, **must** tie to the power-amp's ground; for differential output, can float. See your note below. |
| 10 | TX (GPIO0) | UART TX, 3.3V TTL, 9600-8-N-1 |
| 11 | RX (GPIO1) | UART RX, 3.3V TTL, 9600-8-N-1 |
| 7 | **WAKEUP (GPIO15)** | **Power on/off.** Long-press (6s) powers off; short-press powers on; powers on by default at boot. This is the module's equivalent of the BM64's MFB — the one control pin that genuinely has to be a GPIO, since nothing can reach the module over UART before it's powered up. |
| 6 | PP/CALL (GPIO4) | Button: short-press play/pause/answer/hang-up, long-press reject, long-press (6s) re-pair. **Redundant** — the binary UART protocol has direct equivalents (`BT_CMD_ACK`/`HFHUNG`/`NACK`/`REDIAL`, cmds 0x04–0x07, already in this doc above). Leave unrouted. |
| 12 | MODE (GPIO7) | Button: short-press connect/disconnect toggle. **Redundant** — UART `BT_CMD_CONNECTED`/`DISCONNECTED`/`MATCH` (cmds 0x01–0x03) do the same. Leave unrouted. |
| 13 | S/R (GPIO6) | **Transmit/receive mode *indicator* — an output, do NOT tie it to GND.** The reference schematic's own note: `S/R：发射模式输出高，接收模式输出低` ("outputs high in transmit mode, low in receive mode"), and the reference design drives a green LED from it through 1 kΩ. *Transmit* = broadcast a line-in source out over BT (a "wireless speaker transmitter dongle" feature this project doesn't want); *receive* = normal BT audio/HFP sink (what we want). **The module boots in transmit mode** (`上电开机，默认发射模式`), so firmware has to switch it — UART `BT_CMD_RECMODE` (0x25). Whether that sticks across a power cycle is unknown (bench test); S/R is how you check. The English manual's line for this pin is a garbled translation ("Power-on mode launch mode / short press transmit/receive mode switch") — an earlier version of this doc read it as a boot strap and said to ground it, which would short an output that drives high at boot. |
| 19 | STAT (GPIO10) | Connection status: high when linked, low when not. **Redundant** — the UART link already reports this asynchronously (status bytes 0x64–0x7D, this doc's event table above), the same pattern `bt_companion.c` already uses for the BM64. Leave unrouted. |
| 27 | AUX_R | Transmit-mode audio input, right. Not used — this project is receive-only (see S/R above). |
| 28 | AUX_L | Transmit-mode audio input, left. Not used. |
| 29 | MICN | Mic input, differential negative. **Phone line → BT audio out** goes here (together with pin 30) — this is the SLIC output path, same role as BM64's mic input. |
| 30 | MICP | Mic input, differential positive. |
| 31 | VMIC | Mic bias voltage (for a raw electret capsule). Not needed here — we're feeding it line-level audio from the SLIC, not a bare mic element. |
| 32 | AUDIOLN | Speaker/DAC output, left negative. **BT → phone line audio in** goes here (together with pin 33) — this is the path to Line0Vin. |
| 33 | AUDIOLP | Speaker/DAC output, left positive. |
| 34, 35 | AUDIORP, AUDIORN | Same as 32/33, right channel — spare, this project only needs one (mono) channel. |
| 38 | VCC3YS | **3V output** (not an input — see VBAT above) |
| 39 | VDD3IO | **3V output**, gated off when the module is shut down |
| 36 | VBUS | USB 5V charging input — "Unused" per the datasheet for this application |
| 17, 18, 20, 21, 22–24, 26 | NC, SD_CMD, SD_DAT, SD_CLK, duplicate S/R & MODE pins, GPIO2/3 | **Unused.** The SD pins are exactly what you guessed — a standalone-MP3-player-from-microSD mode that exists on other BK3266 family members but is listed "Unused" on this SR variant. 22/23 are just the same S/R/MODE signals as 12/13, broken out a second time on different pads — not separate functions. |

**AGND tied to the shared ground:** reasonable call for a first spin, and
not really a corner-cut — the datasheet's own guidance for single-ended
output is "must tie to the power amp's ground to suppress noise," which on
a one-ground-plane daughter board *is* what you're doing, just without a
separate star point back to the main board. If it turns out noisy, the
datasheet's fallback for the differential case is "can float" — so the
other direction to try before adding a second ground pin in the next spin
is leaving AGND **unconnected** rather than tied, if tying it ever measures
worse than floating it.

## Wiring plan: daughter-board header (mirrors the BM64 carrier's 8-pin J12)

```
Header pin   Net                                    BK3266SR pin
──────────   ───                                     ────────────
1            +3.3V                              ───► VBAT (37)
2            Audio in  (BT → phone line)         ◄── AUDIOLP (33)*
3            Audio out (phone line → BT)         ───► MICP (30)*
4            UART TX                             ◄── TX (10)
5            UART RX                             ───► RX (11)
6            GND                                 ───► GND (1/3/16/25), AGND (40)
7            WAKEUP (power on/off)               ───► WAKEUP (7)
8            spare

* Differential pins (AUDIOLP/AUDIOLN, MICP/MICN) — only one leg needs to
  reach the header; terminate the complementary leg (AUDIOLN / MICN)
  locally on the daughter board, same trick BM64's carrier doesn't need
  but is standard practice for converting a differential pin pair to a
  single wire. Needs a bench check: confirm the module doesn't need to be
  told (via EEPROM/ToolKit, not the SR binary protocol — that command set
  has no DAC/MIC single-ended switch in it) to treat these as
  single-ended before this works cleanly; some BK3266 firmware variants
  have a software DACSINGLE/MICSINGLE switch for this (see "Two firmware
  variants" above), the SR binary protocol doesn't expose it as a runtime
  command.
```

**GPIO15 is reused directly** — same dedicated MFB-style pin SMD5 already
carved out for the BM64's MFB (`BOARD_BT_MFB_PIN`/`BOARD_BT_MFB_DEDICATED`
in `board_pins_smd4.h`), since both modules need exactly one power/wake
GPIO and nothing else.

**Net result: same control-pin count as BM64.** BM64 needs MFB (GPIO) +
UART for everything else. BK3266SR needs WAKEUP (GPIO) + UART for
everything else too — PP/CALL, MODE, STAT all have direct UART equivalents
already in this doc's command tables above, and receive mode is selected
with UART `BT_CMD_RECMODE` (0x25) after boot. S/R is an output (see the
pin table) — leave it off the main header, and don't tie it to anything.
The header has a spare 8th pin, same as BM64's J12.

**MICN:** 1 µF to GND **on the daughter board**, same as the BM64 carrier's
`MIC_N1` (C24) — an AC ground, not a hard tie, since the input is
internally biased. Keeps it off the header.

**Pin-description conflicts to resolve on the bench:** the reference
schematic's Chinese notes and the English manual's table disagree on which
button does what. Schematic: `MODE` = short-press TX/RX switch, `WAKEUP` =
short-press disconnect/reconnect, 6 s press power off. Manual: `MODE` =
connect/disconnect, `S/R` = TX/RX switch. The buttons are **active-high**
per the schematic (`按键说明：（高电平有效）`), so "pressing" one means a
momentary pulse to 3.3 V, not to GND.

## Diagnostic header on the daughter board (1×6, unpopulated)

Probe and tweezer access to the four signals worth reaching on an
unverified module. No ESP32 GPIOs used. **No shunts, ever**: the buttons
are active-high, so a shunt to GND does nothing, and one to 3.3 V holds
the button down — a 6 s hold powers off or re-pairs.

| Header pin | Signal | Module pin | Note |
|-----|--------|-----|------|
| 1 | S/R | 13 | Output — which mode it's in. 1 kΩ in series at the header |
| 2 | STAT | 19 | Output — link state, independent of the UART parser. 1 kΩ in series |
| 3 | GND | — | Scope/meter reference |
| 4 | MODE | 12 | Input — manual pairing/mode test; settles the conflict above |
| 5 | COM | — | 3.3 V through 10 kΩ, copied from the reference board's `R6` |
| 6 | PP/CALL | 6 | Input — manual answer/hang-up if the UART commands misbehave |

"Pressing" a button = bridging COM to its neighbour (MODE or PP/CALL) with
tweezers or a jumper wire; the 10 kΩ makes a probe slip harmless. The
series 1 kΩ on S/R and STAT does the same for the outputs and doesn't
affect a meter or scope. Optional: two unpopulated tactile-switch
footprints, COM → MODE and COM → PP/CALL, as on the reference board.

**LED (pin 14):** the module **sources** current — reference design has
the LED anode on the pin and the cathode to GND through 1 kΩ (one 1 kΩ
shared with the S/R LED there). ~1 mA with a green LED. Give it its own
LED + 1 kΩ on the daughter board rather than a header pin.

Configure after power-up (if using the text AT-command firmware instead of
the binary SR protocol — see "Two firmware variants" above):
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

## Reference material gathered (2026-10-01)

Pulled from two GitHub collections of Chinese Bluetooth-audio module docs:
[bondango/Bluetooth-Audio](https://github.com/bondango/Bluetooth-Audio) (the
superset) and
[tomaskovacik/kicad-library](https://github.com/tomaskovacik/kicad-library),
whose `library/datasheet/F-6988_BK3266/` folder carries **machine-translated
English** versions of the same manuals. `bondango/China-Blue-tooth-Audio-Devices`
turned out to be a 16-file subset of the first, with nothing unique.

Everything lives in `reference_docs/`:

| File | What it is |
|------|------------|
| `BK3266_module_manual_RGB-LED_ENGLISH.pdf` | **Start here.** Module manual, RGB-LED variant, translated |
| `BK3266_module_manual_IR-remote_ENGLISH.pdf` | Same manual, IR-remote variant, translated |
| `BK3266SR_module_manual_ENGLISH.pdf` | BK3266**SR** (transmit+receive) manual, translated |
| `BK3266SR_serial_protocol_V1.0.0_ENGLISH.xls` | Serial command protocol, translated |
| `BK3266_module_manual_V1.0_*_HYT.pdf` | The Chinese originals of the two manuals |
| `SCH_BT_BK3266_V2.0.pdf` | Module schematic, plain BK3266 |
| `SCH_BT_BK3266SR_V3.0.pdf` | Module schematic, SR variant |
| `BK3266SR_reference_design_V1.0_HYT.pdf` | Reference design |
| `BK3266_connection_diagrams_HD.rar` | 14 MB of high-res wiring diagrams: amp pairings (PAM8610, HT8696, HT6872, SGM4812), 5 V / 12 V / Li-ion supply options, and a common-ground noise fix |
| `BK_family_footprint_AD.PCBLIB` | **Altium** footprint library, BK8002/3254/3260/3266 |
| `BK3266_AT_command_reference.rar`, `BK3266_serial_debug_assistant.rar` | Command docs and a Windows serial terminal (sscom) |

The vendor's own board name for the BK3266 module is **F-6988** (the
BK8000L's is F-6188) -- useful when searching, since many listings use the
board number rather than the chip.

## No KiCad symbol or footprint exists for the BK3266

Checked 2026-10-01: `tomaskovacik/kicad-library` has BK-family *datasheets*
but no `.kicad_mod`/`.kicad_sym` for any BK part, and nothing else turned up.
The only footprint data anywhere is `BK_family_footprint_AD.PCBLIB`, which is
**Altium**.

Three ways forward, cheapest first:

1. **Draw it from the manual.** The module is a through-hole/castellated
   board with a couple of dozen pads on a regular pitch -- an hour's work in
   the footprint editor, and the dimensions are in the English manual above.
   This is the honest answer for a one-off.
2. **Convert the Altium library.** KiCad can import Altium *boards*, but
   `.PCBLIB` footprint libraries are not a supported import path in the
   versions this project uses, so this would mean a third-party converter
   and then checking every pad by hand anyway.
3. **Trace the reference schematic.** `SCH_BT_BK3266_V2.0.pdf` gives the
   pinout for the symbol; the symbol is the easy half either way.

**Note:** `Bluetooth/BK3266_board_v1/` is currently still a copy of the BM64
carrier -- its schematic and PCB reference `BM64:BM64_MCH` and
`BM64SPKS1MC2-0003AA`, so no BK3266 part has been placed yet.
