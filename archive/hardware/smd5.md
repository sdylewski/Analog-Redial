# Analog Redial v5 (SMD5) — dev notes

> **Status: sent to fab, not yet tested.** Ordered together with the BM64
> Bluetooth carrier (`Bluetooth/Bluetooth_board/`) and a BK3266 test board
> (`Bluetooth/BK3266_board_v1/`) for a first look at that module as a BM64
> alternative.

KiCad project: `hardware/kicad/Analog Redial v5/`. Working notes per
[`CLAUDE.md`](../../CLAUDE.md)'s public-vs-internal-docs split; the public
reference is [`docs/hardware-smd5.md`](../../docs/hardware-smd5.md).
Everything not listed here is the same as SMD4 — see
[`smd4.md`](smd4.md) and
[`docs/hardware-smd4.md`](../../docs/hardware-smd4.md).

## What's different from SMD4

- **Dual MCU footprint** (`U1`): takes a genuine Espressif
  ESP32-C6-DevKitC-1 or a cheaper AliExpress clone, selected with two solder
  jumpers (JP2, JP3).
- **BM64 Bluetooth header** `J2`, 8 pins, with its own 3.3 V LDO (U5) and
  MFB on GPIO15.
- **SHK divider 10 kΩ / 100 kΩ** (R15/R14 line 0, R13/R12 line 1).

## Dual MCU footprint

The schematic has one symbol (`PCM_Espressif:ESP32-C6-DevKitC-1`, real
module's pad numbering); the footprint is what's dual-compatible. The two
modules disagree at four pad positions:

| Footprint pad | Genuine module | Clone module |
|--|--|--|
| 12 | `GPIO2` | `GPIO12` / `USB_D-` |
| 13 | `GPIO3` | `GPIO13` / `USB_D+` |
| 19 | `GPIO12` / `USB_D-` | `GPIO2` |
| 20 | `GPIO13` / `USB_D+` | `GPIO3` |

The ESP32-C6's native USB is fixed to GPIO12/13 in silicon, so whichever
pads those land on carry live USB on that module. Other duplicate pad
entries in the `.kicad_mod` files (15, 18, 22–31) are positions where both
vendors agree.

### JP3 / JP2: MCLK and card-detect select

| Jumper | Net | Pad 1 (default, bridged) | Pad 3 (clone) |
|--|--|--|--|
| **JP3** | `I2S_MCLK` → codec `MCLK` (U2 pin 1) | footprint pad 12 | footprint pad 19 |
| **JP2** | `SD_CD` → Card1 pin 9 | footprint pad 13 | footprint pad 20 |

Set to match the module, both nets land on GPIO2/GPIO3 and the normal
`CONFIG_BOARD_TYPE_SMD5` build works on either module with native USB.
To switch: cut the 1–2 trace, bridge 2–3. Bridge one side only — both
sides together short GPIO2 to a USB data line.

**Wrong jumper: nothing errors.** The codec still answers on I²C
(`codec detect: ES8388 @ 0x10`, `es8388: init ok`), Wi-Fi, web UI, hook
detection and ringing all work, but there's no audio at all and microSD
always reads "no card".

| Module | Jumpers | Firmware | Result |
|--|--|--|--|
| Genuine | default | `SMD5` | works |
| Clone | clone side | `SMD5` | works |
| Clone | default | `SMD5_CHEAP` | works, but no USB console (see below) |
| Clone | default | `SMD5` | no audio, microSD "no card" |
| Clone | clone side | `SMD5_CHEAP` | no audio, no USB console |
| Genuine | clone side | `SMD5` | no audio; MCLK and card-detect sit on USB D−/D+, so a seated card stops USB enumerating |

Fastest check: scope U2 pin 1 for 12.288 MHz. Present at the ESP32 pad but
not at the codec means the jumper is on the wrong side.

**J3/J4** (spare 2-pin headers) are on footprint pads 20/19: live USB
D+/D− with the genuine module. Leave them unconnected on those boards.

**KiCad symbol note** (for future spins): use the stock
`Jumper:SolderJumper_3_Bridged12` symbol — its pins really are 1/2/3, with
letters A=1, C=2, B=3, so wire by pin number. SparkFun's
`SolderJumper_3_Bridged12_No_Silk` symbol numbers B and C both as pin 2.
Footprint: `Jumper_3_NC-1_Trace_NoSilk` (only pads 1–2 joined).

### `SMD5_CHEAP` build (clone module, jumpers at default)

Only for a clone board whose jumpers were left at the default, so MCLK and
card-detect are on the clone's GPIO12/13:

- `board_pins_smd4.h` maps MCLK → GPIO12, CD → GPIO13, and
  `board_early_pin_init()` detaches the USB PHY from those pads before
  audio starts. For ~1 s after reset the ROM owns them.
- No USB console: the port disappears ~1 s into every boot. Use the web
  UI, or UART0.
- Flashing over native USB still works (the ROM owns the pads at reset).
  The `download` console command restarts into the ROM; power-cycle when
  flashing is done. BOOT + RESET does the same in hardware.
- A seated microSD card holds USB D+ low (card-detect shorts to GND), so
  USB can't enumerate. Pull the card before flashing.
- `sdkconfig.smd5_cheap.defaults` puts the console on UART0 (GPIO16/17),
  the same pins as the BM64 UART on J2 — they can't share.
- Don't plug a cable into the clone module's own USB-C while the app runs:
  MCLK is on its D− pin.

UART0 flashing (GPIO16 TX, GPIO17 RX, EN, GPIO9 boot strap, with a 3.3 V
USB-serial adapter) works on any module and is the fallback if firmware
won't come up far enough for `download`.

## BM64 Bluetooth header (J2)

8 pins, mating with `J1` on the carrier. Both boards' netlists agree:

| J2 pin | Main board | Carrier J1 → BM64 |
|--|--|--|
| 1 | `+3.3V_BM` from **U5** | `BAT_IN` |
| 2 | GPIO17 / U0RXD (ESP receives) | `HCI_TXD` |
| 3 | GPIO16 / U0TXD (ESP transmits) | `HCI_RXD` |
| 4 | `MFB` → GPIO15, **R17** 100 kΩ to GND | `MFB` |
| 5 | **C22** 1 µF ← `Line0Vout` (SLIC U4 pin 10) | `MIC_P1` |
| 6 | **C24** 1 µF ← GND | `MIC_N1` |
| 7 | **C23** 1 µF → **R16** 1 kΩ → `Line0Vin` | `AOHPL` |
| 8 | GND | GND |

Audio is analog between the BM64 and line 0's SLIC; the ESP32 only drives
the UART and MFB (`bt_companion_line()` always returns line 0). Pin 1 is
3.3 V and pin 8 GND, so a board mated 180° round shorts the LDO. Parts for
this option: [`BOM-bluetooth.md`](BOM-bluetooth.md).

- **Supply:** U5 (XC6206P332MR, +5 V → 3.3 V, C26/C25 1 µF) gives the BM64
  its own rail, isolated from the module's regulator. BAT_IN spec is
  3.2–4.2 V; draw is ~0.6 mA standby, ~16 mA in a call, with short RF peaks
  well above that.
- **MFB** is the BM64's power button. Held high from power-up it triggers a
  master reset that wipes pairings, so R17 holds it low while the ESP32
  boots. Firmware drives it per the datasheet (Fig 5-3/5-4): high 400 ms
  to power on, then low, with a 1 ms wake pulse and ≥150 ms high around
  each UART command. GPIO15 is a strapping pin only for JTAG source select
  with an eFuse that ships unburned, so the pull-down is harmless; GPIO8
  (RGB LED) and GPIO9 (download strap) weren't usable.
- **RST_N** stays unconnected in normal use (internal power-on reset). It's
  only connected for EEPROM programming.
- **R16 (1 kΩ)** limits fault current into the SLIC's Vin from the BM64
  output. C23 at 1 µF puts the corner under ~30 Hz into Vin's ~60 kΩ.
- **Keep line 0's C12 (codec → Vin) at 10 nF.** During a Bluetooth call the
  codec's LOUT1 is a low-impedance output, so C12 shunts the BM64's signal:
  10 nF costs ~1.5 dB at 3 kHz through R16, 100 nF would cost ~9 dB. Line
  1's C15 has no BM64 branch and isn't affected.

### BM64 EEPROM programming (through J5)

The BM64's IBDK mode needs RST_N and P2_0 driven at reset — a one-time step
per module. The carrier's 4-pin `J2` (`P2_0`, `RST_N`) mates with the main
board's J5 for this:

1. Pull J5's four shunts (line 1's SLIC, if fitted, is then disconnected).
2. Plug the carrier's programming header into J5.
3. Settings → Hardware: one line, BM64 fitted. Run the EEPROM check.
4. Unplug and refit the shunts.

`bt_companion_ibdk_enter()` drives GPIO18–20 for this and refuses on a
board configured for two lines.

**Holding the programming header in place.** A 2.54 mm header pin is a
0.64 mm square post (~0.90 mm across the corners). A snug fit needs a
~0.9 mm drill, but JLCPCB's ±0.08 mm plated-hole tolerance is bigger than
the interference, so friction isn't repeatable. Options: stagger alternate
holes 0.05–0.15 mm off the centreline (grip then comes from bending the
pins, set by hole position, which is held more tightly than diameter), or
tack-solder two end pins. Since a dropout mid-write leaves the module
half-configured, tack-soldering is the safe default.

## SHK divider

10 kΩ series, 100 kΩ to ground, both lines. The AG1171's SHK output is
~3.3 V through an internal 3.3 kΩ pull-up (VOH 2.8 V min); with 100 kΩ the
GPIO sees ~2.9 V off-hook, above the C6's 2.475 V input-high threshold, and
a defined low with the SLIC powered down.

## Open questions

- **Clone module regulator.** The clone's onboard 3.3 V regulator is
  smaller than the genuine module's. With the BM64 on U5, it supplies the
  MCU and codec; measure worst-case draw (Wi-Fi TX bursts + codec) on a real
  clone board before relying on it.
- **Clone pad map.** Only the 12/13 ↔ 19/20 swap was checked against a real
  sample (one clone's silkscreen). Verify pad by pad (continuity, or a
  GPIO-toggle build) before ordering a batch.
- **Power sequencing** between the BM64 and the SLICs — see
  [`archive/BLUETOOTH_AUDIO_UPGRADE.md`](../BLUETOOTH_AUDIO_UPGRADE.md).

## Ideas not on this board

- **A multi-function button** on a spare ESP32 GPIO (not on MFB): click to
  start Bluetooth pairing, ~3 s hold for the rescue Wi-Fi network, ~10 s
  to forget Wi-Fi — the same gestures `board_boot_button_start()` already
  handles for BOOT. A side-mounted tactile switch, reachable through the
  enclosure wall.
