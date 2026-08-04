# Rotary Phone Gateway — SMD3 board

Design reference for the **third** SMD spin. This doc only covers what's
**different from SMD2** — everything else (ES8388 pin table, AG1171 SLIC
wiring, GND/audio path, JLC assembly notes) still applies from
[`ROTARY_PHONE_SMD.md`](ROTARY_PHONE_SMD.md) unless noted here.

KiCad project: `hardware/kicad/Rotary_phone_SMD3/`.

## Status

In progress — schematic/layout work just starting. This doc tracks
decisions as they're made, not a finished design.

## What's different from SMD2

| | SMD2 | SMD3 |
|--|------|------|
| MCU | ESP32-C6-WROOM-1 module, hand-soldered (castellated pads) | **ESP32-C6-DevKit** (complete dev board — own USB-C, reset button, onboard 3.3V LDO), socketed/header-mounted |
| Power in | USB-C only (VBUS → onboard AP2112K-3.3 LDO) | USB-C (via the DevKit's own port) **or** a new **DC jack**, diode-OR'd — see [Power](#power) |
| 3.3V rail | Dedicated AP2112K-3.3 LDO on the PCB | Borrowed from the **DevKit's own 3.3V output pin** — no separate LDO for this rail (see [Power](#power)) |
| Main driver | ESP32 only (WROOM module) | ESP32-C6-DevKit (same driver, different form factor) — Pi-header option considered and dropped, see below |
| SLIC VIN/VOUT wiring | **Known bug**: coupling caps swap SLIC Vin/Vout vs. codec (bodge-fixed on hand-soldered boards) | **Fix in schematic from day one** — see [Carried-forward SMD2 fixes](#carried-forward-smd2-fixes) |
| Mono ADC input | **Known bug**: both lines shared one ES8388 ADC channel (`LINSEL` mux) | **Fix in schematic from day one** — route each line's mic to its own dedicated ADC channel (`LIN1`/`RIN1`) |

## Power

### DC jack + USB-C isolation

The DevKit has its own USB-C port (feeding its own onboard 5V→3.3V
regulation) — SMD3 adds a **DC jack** as an alternative 5V input, both
landing on the same board-level +5V net that also feeds the AG1171 SLICs'
`+VPWR`.

**Needs a series diode on the DC jack input.** Two live 5V sources tied
to the same net without isolation means current can flow backward between
them — worst case, back up the USB cable into whatever's on the other end,
or into the DC jack's supply if it doesn't tolerate being back-fed.
Standard fix: a Schottky diode (same class as `D1` `SS14` already used
elsewhere in this project — see [ROTARY_PHONE_SMD.md USB-C section](ROTARY_PHONE_SMD.md#usb-c-receptacle-usb1))
in series between the DC jack and the +5V net, cathode toward +5V. This
diode-ORs the two sources: whichever is present (or higher) wins, neither
can push current into the other.

**Headroom note:** the diode drops ~0.3-0.5V. If the DevKit's onboard
3.3V regulator needs meaningful headroom above 5V minus that drop, spec
the DC supply at 5.5-6V rather than exactly 5V — check the specific
DevKit's onboard regulator datasheet for its minimum input voltage before
finalizing.

### 3.3V rail — borrowed from the DevKit, not a separate LDO

Unlike SMD2 (dedicated AP2112K-3.3 on the PCB), SMD3's plan is to use the
DevKit's own 3.3V output pin to power just the board's 3.3V rail — which
on this board means just the **ES8388** (plus its I2C pull-ups). No
separate LDO needed.

## Raspberry Pi header — considered, dropped

An earlier revision of this doc planned a 40-pin Raspberry Pi header as
an alternative main driver to the ESP32-DevKit (one or the other,
open-hardware "pick your own MCU" board). Dropped due to complexity —
**SMD3 is ESP32-only.** If this gets revisited for a future spin, the
pin-mapping research (I2S0/I2C1/SPI0 fixed-position constraints on the
Pi's 40-pin header) is preserved in git history on this file.

## Carried-forward SMD2 fixes

Both of these were found on the hand-soldered SMD2 board and bodge-fixed
there (see [`ROTARY_PHONE_SMD.md`'s "Signal flow" section](ROTARY_PHONE_SMD.md#signal-flow-audio-path)
for full root-cause detail) — SMD3's schematic needs to have the *correct*
wiring from the start, not carry the bodges forward as the "real" design:

1. **SLIC Vin/Vout swap**: coupling caps had codec `LIN1`/`LIN2` (ADC
   inputs, passive) landing on the SLIC's **Vin** pin (needs an external
   driver) and codec `LOUT1`/`ROUT1` (DAC outputs, actively driven)
   landing on **Vout** (the pin the SLIC itself drives) — backwards on
   both lines. Fix: swap the net assignment so `LOUT1`/`ROUT1` connect to
   each SLIC's `Vin` and `LIN1`/`LIN2` connect to each SLIC's `Vout`.
2. **Mono ADC input**: both lines' mic signals shared one ES8388 ADC
   channel via the `LINSEL` mux, so only one line's mic was ever
   capturable during a bridge. Fix: route each line's mic into its own
   dedicated ADC channel from the start (line 1 → `LIN1`, line 2 →
   `RIN1`) instead of both landing on `LIN1`/`LIN2`.

## Open questions

- **DevKit part choice**: specific ESP32-C6-DevKit model/vendor not yet
  pinned down — confirm its onboard regulator's minimum 5V input headroom
  once chosen (affects the DC jack voltage spec above).

## See also

- [`ROTARY_PHONE_SMD.md`](ROTARY_PHONE_SMD.md) — SMD2, still the reference
  for everything not called out as different above (ES8388 pin table,
  AG1171 SLIC/RJ12 wiring, GND plane, JLC assembly notes).
- [`HARDWARE_SIGNAL_LEVELS.md`](../HARDWARE_SIGNAL_LEVELS.md) — SLIC
  levels, SHK dividers.
