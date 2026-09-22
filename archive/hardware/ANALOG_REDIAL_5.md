# Analog Redial 5 board (SMD5) — dev notes

KiCad project: `hardware/kicad/Analog Redial 5/`. This is a work-in-progress
board, not yet fabricated as of this writing (2026-09-22) — these are
working notes per [`CLAUDE.md`](../../CLAUDE.md)'s public-vs-internal-docs
split, not a public build guide. Nothing here is final until boards come
back and get bench-tested, same caveat as `ROTARY_PHONE_SMD4.md`.

## What's different from SMD4

The headline change is the MCU module footprint: SMD5 accepts **either** a
genuine Espressif ESP32-C6-DevKitC-1 module **or** a cheaper AliExpress
clone module with a different physical pinout, via a dual-compatible
footprint (`Scott.pretty/ESP32-C6-DevKitC-dual.kicad_mod` for the real
module, `ESP32-C6-DevKitC-cheap.kicad_mod` for the clone). Everything else
(SLIC/codec/tip-ring layout, clearance rules) carries over unchanged from
SMD4 — see [`ROTARY_PHONE_SMD4.md`](ROTARY_PHONE_SMD4.md) and
[`docs/hardware.md`](../../docs/hardware.md) for that baseline.

## The module swap: what actually changes

Both module types are electrically the same chip (ESP32-C6), so the
schematic itself (`Analog Redial 5.kicad_sch`) only needs **one** symbol
(`PCM_Espressif:ESP32-C6-DevKitC-1`, ref `U1`) using the real module's
standard pad numbering. The clone module's PCB routes some of its own chip
pins to *different physical pad positions* than the real module does — the
footprint (not the schematic) is what's dual-compatible, by physically
placing each shared pad number at whichever location lines up with *both*
modules' silkscreens, wherever they agree.

**The one confirmed discrepancy** (found by directly comparing the two
modules side by side): pad positions 12/13 and 19/20 disagree between
vendors.

| Schematic pad # | Real Espressif module | AliExpress clone module |
|--|--|--|
| 12 | `GPIO2` | `GPIO12` / `USB_D-` |
| 13 | `GPIO3` | `GPIO13` / `USB_D+` |
| 19 | `GPIO12` / `USB_D-` | `GPIO2` |
| 20 | `GPIO13` / `USB_D+` | `GPIO3` |

In other words: wherever the real module's native-USB pins physically are,
the clone's `GPIO2`/`GPIO3` show up instead, and vice versa. **This is a
silicon-level constraint, not a Kconfig one**: on the ESP32-C6, the
USB-Serial-JTAG *peripheral* is hard-wired to GPIO12 (D-)/GPIO13 (D+) —
unlike UART/SPI/I²C, which route through a flexible GPIO matrix to whatever
pin you configure, native USB cannot be relocated to different pins in
software. That's exactly why Espressif's own schematic labels these pins
"GPIO12"/"GPIO13" first (with the USB function as a secondary annotation,
same as this project's own symbol does above) — they're ordinary GPIOs any
time the native-USB peripheral isn't in use, which is the normal case here.

**Practical consequence**: whichever physical pad position ends up wired to
this board's actual USB-C/micro connector will only carry real native-USB
signaling with the real Espressif module populated. With the AliExpress
clone installed in the same footprint, that same connector position instead
carries the clone's `GPIO2`/`GPIO3` — ordinary digital pins, not USB — so
the board's own USB port goes data-dead (still fine as power-only) on
clone-populated units. No electrical risk either way (both are just 3.3V
digital pins), just a functional loss of native-USB flash/console on that
variant.

**Layout rule this implies**: don't assign anything else important to pad
positions 12/13/19/20 in this footprint — a clone-populated board puts live
USB D-/D+ signaling on whichever of those positions its real GPIO12/13 land
at, so anything else routed there risks interference.

**Other duplicate pad-number entries in the `.kicad_mod` files** (15, 18,
22-31) are *not* additional known discrepancies — they're just how the dual
footprint records that both vendors happen to agree at that position. Only
the 12/13 ↔ 19/20 swap above is a confirmed, deliberately-checked mismatch.
Before trusting this for a production run, re-verify pad-by-pad against a
real sample of the clone module (continuity/multimeter check, or flash a
firmware build that toggles a known GPIO and probe the physical pad) — a
dual-footprint pad remap like this is inherently easy to get subtly wrong,
and this project has exactly one clone-module sample's silkscreen to go on
so far.

## Flashing/console tradeoff for clone-populated boards

Because native USB dies (as data) on a clone-populated board, don't rely on
it for flashing or the interactive console there — see
[`CLAUDE.md`](../../CLAUDE.md)'s "Building/flashing for a specific board
type" section for the general multi-board-type gotchas, none of which are
specific to SMD5. The concrete plan for SMD5:

- **Flash over UART0 instead**, which works identically regardless of which
  module is populated: `GPIO16` (U0TXD) / `GPIO17` (U0RXD) — same pins this
  project already reserves as `BOARD_UART_TX`/`BOARD_UART_RX` for the BM64
  companion link (see below) — plus `EN` (reset) and `GPIO9` (boot-mode
  strap, pulled low at reset to enter download mode). An external
  FTDI-style USB-serial adapter on those four signals (+3V3/GND) drives
  `esptool`/`idf.py flash` exactly like the native-USB path does on
  SMD2/SMD4, and also gives a working `idf.py monitor` console any time —
  not just during the initial flash.
- **Plan**: flash + bench-test each board over that UART0 connection before
  or right after assembly (this is where bugs get caught, same commands as
  any other board type), then rely on OTA (`/ota_upload`) for every update
  after that, once firmware is confirmed good. Don't ship untested firmware
  to a clone-populated board expecting to debug it later over its own USB
  port — that port won't be reachable once assembled.
- This doesn't need a new `CONFIG_BOARD_TYPE_SMD5` Kconfig choice by itself
  — the pin map (SLIC/codec/etc) is identical to SMD4's; only the
  flash/console *method* differs, and that's a bench-workflow choice, not
  something firmware needs to know about at runtime.

## BM64 Bluetooth companion header (J12)

SMD5 adds a 6-pin board-to-board header (`J12`, `PINHD_1x6_Male`,
`Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical`) for the BM64
Bluetooth Classic audio companion daughter-board — see
[`archive/BLUETOOTH_AUDIO_UPGRADE.md`](../BLUETOOTH_AUDIO_UPGRADE.md) for
why this exists (ESP32-C6 has BLE only, no Classic BT) and
`firmware/components/bt_companion/` for the UART protocol implementation
that talks to it (confirmed working against real BM64 hardware 2026-09-21/22
— see that component's own doc comments for the full protocol writeup).

Traced directly from the schematic (`Analog Redial 5.kicad_sch`):

| J12 pin | Net | Confirmed via |
|--|--|--|
| 1 | GND | direct `power:GND` symbol on this pin |
| 2 | `GPIO16`/`U0TXD` (ESP32 TX → BM64 RX) | wire-traced to `U1` pin 31 |
| 3 | `GPIO17`/`U0RXD` (ESP32 RX ← BM64 TX) | wire-traced to `U1` pin 30 |
| 4, 5 | a second GND/3.3V pair, each with a local decoupling cap at the header | wire-traced to two `C_Small` capacitors near J12; which of 4/5 is GND vs 3.3V wasn't independently confirmed — check visually in KiCad before relying on it |
| 6 | +3.3V | direct `power:+3.3V` symbol on this pin |

So: 2 dedicated UART pins (TX/RX) plus two GND/3.3V pairs for a robust
power connection to the daughter board — no MFB (module power-on/button)
pin is broken out to this header, matching `bt_companion.c`'s UART-only
design (see its own header doc comment). If a future revision wants
firmware-controlled MFB (to support commanding the module to power off over
UART via `BM64_MMI_POWEROFF_BT` and later waking it back up), that needs an
extra conductor added to this header or a separate one — a fixed
resistor-to-VCC on MFB (the current bench-test wiring) works to get the
module running but gives no way back once powered off.

## Open questions

- Full pad-by-pad diff between the two module footprints hasn't been
  independently re-verified beyond the one 12/13↔19/20 swap found by direct
  inspection — worth doing before ordering a batch of boards.
- No `CONFIG_BOARD_TYPE_SMD5` exists yet; add one only if SMD5 ends up
  needing a different pin map from SMD4, not just a different flash
  workflow.
- MFB wiring (fixed pull-up vs. GPIO-controlled) is still an open bench
  question — see the Bluetooth companion section above and
  `archive/BLUETOOTH_AUDIO_UPGRADE.md`.
