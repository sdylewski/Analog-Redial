# Analog Redial 5 board (SMD5) — dev notes

> **Status (2026-09-27): work in progress, not fabricated, not tested.**
> This is the planned next generation, not a board anyone should build from
> yet. Nothing below has been validated on real hardware, and the BM64
> header section describes a planned re-route that isn't in the KiCad files
> yet. Firmware supports it only because SMD5 shares SMD4's pin map
> (`board_pins.h`) -- that sharing will need revisiting once MFB moves (see
> below).

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

## Open concern: clone module's onboard 3.3V regulator may be undersized

Flagged 2026-09-22, not yet measured: the AliExpress clone module carries a
smaller onboard 3.3V regulator than the real Espressif module's. This
board's 3.3V rail has to feed more than just the MCU module itself once the
BM64 companion and ES8388 codec are both wired in through J12/the
audio path — worth checking, before relying on the clone variant for
anything beyond bench testing, whether that regulator can actually supply
everything at once under worst-case load (WiFi TX bursts + codec + BM64
all drawing simultaneously). If it can't:
- The simplest fix is adding a separate/second 3.3V regulator on the main
  board for the codec (and BM64, if its own supply also comes from this
  rail) rather than relying on the clone module's onboard one for
  everything.
- If that's not workable, the practical fallback is: don't populate BM64 on
  a clone-module board at all (real-module boards only for the Bluetooth
  companion feature) until this is measured and resolved.
Needs an actual current-draw measurement on a real clone-module sample
before this can be resolved either way — don't assume either outcome
without measuring.

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
  SMD2/SMD4.
- **Plan**: flash + bench-test each board over that UART0 connection before
  or right after assembly (this is where bugs get caught, same commands as
  any other board type), then rely on OTA (`/ota_upload`) for every update
  after that, once firmware is confirmed good. Don't ship untested firmware
  to a clone-populated board expecting to debug it later over its own USB
  port — that port won't be reachable once assembled.
- **Two Kconfig board types exist for this** (added 2026-09-22, built and
  verified against both — see `firmware/README.md`'s multi-board-type
  section): `CONFIG_BOARD_TYPE_SMD5` (real module, native USB console, same
  as SMD2/SMD4) and `CONFIG_BOARD_TYPE_SMD5_CHEAP` (clone module,
  `sdkconfig.smd5_cheap.defaults` leaves the console on plain UART0 instead
  of overriding to native USB — the one SMD board type where that's
  correct, not a bug). Same pin map as SMD4 either way (`board_pins_smd4.h`)
  — the split is purely about the console/flash story, nothing in the
  active pin map differs yet. Building each needed the `wm8731`/`es8388`
  components' own per-board-type CMakeLists.txt gates extended too (they
  don't follow `board_pins.h`'s routing automatically) — worth remembering
  if a 3rd variant ever gets added.

## BM64 Bluetooth companion header (J12)

SMD5 adds a 6-pin board-to-board header (`J12`, `PINHD_1x6_Male`,
`Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical`) for the BM64
Bluetooth Classic audio companion daughter-board — see
[`archive/BLUETOOTH_AUDIO_UPGRADE.md`](../BLUETOOTH_AUDIO_UPGRADE.md) for
why this exists (ESP32-C6 has BLE only, no Classic BT) and
`firmware/components/bt_companion/` for the UART protocol implementation
that talks to it (confirmed working against real BM64 hardware 2026-09-21/22
— see that component's own doc comments for the full protocol writeup).

Confirmed directly by the board's designer 2026-09-22 (supersedes an
earlier, incorrect trace attempt from the schematic text that had pins 2-5
wrong):

| J12 pin | Net |
|--|--|
| 1 | +3.3V |
| 2 | Line0Vin, via a 10µF series cap (analog audio, network/BM64 → phone line) |
| 3 | Line0Vout, via a 1µF series cap (analog audio, phone line → BM64) |
| 4 | BM64 UART TX |
| 5 | BM64 UART RX |
| 6 | GND |

So this header carries the **full companion interface**, not just UART:
power, both audio directions (analog, per `bt_companion.h`'s own doc
comment — audio never passes through firmware at all), and the UART
control link (`GPIO16`/`U0TXD` and `GPIO17`/`U0RXD` on the ESP32 side, same
pins as every other board type's `BOARD_UART_TX`/`BOARD_UART_RX`). Still no
MFB (module power-on/button) conductor — that's still bench-wired
separately (fixed pull-up to VCC as of 2026-09-21's bring-up; see the
planned re-route below). The "Line0" naming is correct: firmware's
`bt_companion_line()` (`telephony.c`) now always returns line 0, matching
this header (resolved 2026-09-27 -- it had briefly been line 1).

**Companion daughter-board**: `hardware/kicad/Bluetooth_board/` is the BM64
module's own carrier board, meant to mate to J12 with a matching female
header (same 6-pin pinout as above, mirrored).

### Planned re-route (decided 2026-09-27, not yet in KiCad)

1. **Grow J12 by at least one pin to carry MFB**, on its own ESP32 GPIO --
   so normal operation (power-on/wake) is firmware-controlled rather than a
   bench-wired pull-up, and no longer borrows line 1's pins. GPIO not yet
   chosen. **Firmware follow-up:** SMD5 currently inherits
   `BOARD_BT_MFB_PIN = GPIO18` from `board_pins_smd4.h`; once MFB has its own
   pin, SMD5 needs its own `BOARD_BT_MFB_PIN` (and likely its own pin header
   rather than sharing SMD4's).
2. **EEPROM programming goes through J5, not J12.** The BM64's IBDK mode
   needs RST_N and P2_0 (plus MFB) driven at reset -- rare, one-time-per-
   module setup. Rather than route those permanently, the Bluetooth board
   gets a *separate* small header that mates to J5 (the line-1/microSD
   jumper header) just for programming. Procedure: pull J5's four shunts
   (line 2's SLIC, if stuffed, is then electrically disconnected), plug the
   Bluetooth board's programming header into J5, set Settings -> Hardware to
   one line + BM64 fitted, run the EEPROM check, then unplug and re-fit the
   shunts. Firmware already supports this path: `bt_companion_ibdk_enter()`
   drives GPIO18-20 and refuses on a board configured for two lines.
3. **Add the Vin protection network** -- series resistor + clamp diodes to
   +5 V at the Line0Vin tap -- designed in
   [`archive/BLUETOOTH_AUDIO_UPGRADE.md`](../BLUETOOTH_AUDIO_UPGRADE.md)
   after a SLIC's audio hybrid died during BM64 bench work on SMD4. The
   existing 10 µF series cap on J12 pin 2 alone is not protection.

### SHK divider: change 15 kΩ → 100 kΩ (found 2026-09-27)

SMD5 inherits SMD4's 10 kΩ / 15 kΩ SHK divider, which was sized for a 5 V
SHK swing. The AG1171's SHK is actually a ~3.3 V output with a 3.3 kΩ
internal pull-up (VOH 2.8 V min), so the divider lands off-hook at ~1.9 V
-- below the ESP32-C6's 2.475 V input-high threshold. On dualphone2, line
1 (GPIO19) never registered the handset lift at all while line 0 (GPIO6)
worked with the identical voltage, purely by pad-threshold luck. **Keep the
10 kΩ, change the 15 kΩ to 100 kΩ on both lines** (~2.9 V off-hook, still
a defined low with the SLIC powered down). Both values are JLCPCB Basic
parts in common footprints, which was the constraint behind the original
15 kΩ -- confirm at the chosen footprint. Details in `docs/hardware.md`.

## Open questions

- Full pad-by-pad diff between the two module footprints hasn't been
  independently re-verified beyond the one 12/13↔19/20 swap found by direct
  inspection — worth doing before ordering a batch of boards.
- ~~MFB wiring (fixed pull-up vs. GPIO-controlled)~~ -- decided: its own
  GPIO on an expanded J12 (see the planned re-route above). Which GPIO is
  still open.
- Power sequencing between the BM64 and the SLICs -- see the open question
  in `archive/BLUETOOTH_AUDIO_UPGRADE.md`.
