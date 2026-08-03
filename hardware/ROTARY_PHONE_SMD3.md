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
| Main driver | ESP32 only | ESP32-C6-DevKit **or** a Raspberry Pi (40-pin header) — **one or the other**, not both at once. See [Raspberry Pi header](#raspberry-pi-header-compatibility) |
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
separate LDO needed for the ESP32-DevKit case.

**If the Pi is populated instead**, the equivalent would be the Pi's own
3.3V pins (header pins 1/17) — but the Pi's 3.3V rail is officially
budgeted for only **~50mA total** across the whole 40-pin header. The
ES8388 alone (~10-20mA typical) should fit, but there's much less margin
than the DevKit case, and none left over if anything else ever gets added
to that rail. Worth reconsidering (e.g. a small dedicated LDO after all,
just for the Pi-populated case) if the board grows.

## Raspberry Pi header compatibility

The design goal: **one PCB layout, one set of peripheral nets**
(ES8388's I2S+I2C, each SLIC's control GPIO, optionally a shared
microSD), driven by **either** the ESP32-DevKit **or** a Raspberry Pi,
whichever is physically populated. They're never both active — no
cross-wiring is needed *between* the ESP32 and the Pi's own pins, since
they don't talk to each other directly. What matters is that both
driver options land their respective (fixed for the Pi, flexible for the
ESP32) hardware-peripheral pins on the *same* nets going to the shared
peripherals.

The Pi's 40-pin header has **fixed-position** hardware peripherals
(I2C1, SPI0, I2S0, UART0 always live on the same physical pins on every
Pi model with this header — Zero, 3, 4, 5, CM-via-adapter). The ESP32's
GPIO matrix is flexible, so the existing SMD2/SMD3 GPIO choices
(`board_pins_smd.h`) stay as-is; the Pi header just needs its fixed pins
routed to the matching nets.

### I2S (the one that actually matters most — not just I2C)

I2C only carries **codec register control**. The actual **audio samples**
go over **I2S**, and the Pi's I2S0 peripheral is just as fixed-position as
its I2C1 — without this wired correctly, a populated Pi couldn't drive the
codec's audio path at all, only configure it. Prior project research
already worked this mapping out for a different codec (WM8731, Pi Zero 2
W) in [`MCU_VS_PI_ZERO_STACK.md`](../MCU_VS_PI_ZERO_STACK.md) — same
mapping applies here since it's a property of the Pi's SoC I2S peripheral
position, not the specific codec chip.

| Codec pin (ES8388) | Net (matches SMD2 naming) | ESP32-C6 GPIO | Pi BCM | Pi physical pin |
|---|---|---|---|---|
| Pin 1, MCLK | `I2S_MCLK` | GPIO0 | **BCM4** (GPCLK0) | **7** |
| Pin 5, SCLK (bit clock) | `I2S_BCLK` | GPIO4 | **BCM18** | **12** |
| Pin 7, LRCK | `I2S_LRCK` | GPIO5 | **BCM19** | **35** |
| Pin 6, DSDIN (codec DAC in ← MCU out) | `I2S_DOUT` | GPIO6 | **BCM21** (Pi's own "DOUT") | **40** |
| Pin 8, ASDOUT (codec ADC out → MCU in) | `I2S_DIN` | GPIO7 | **BCM20** (Pi's own "DIN") | **38** |

**Naming gotcha:** "DIN"/"DOUT" flip meaning depending whose datasheet
you're reading (MCU-centric vs. Pi-centric) — the table above is
organized by **codec pin**, which is unambiguous. Double-check against
that column, not against "DIN"/"DOUT" labels alone, when actually wiring
this.

**MCLK caveat:** the Pi generates MCLK via a general-purpose clock output
(GPCLK0), not a dedicated audio clock — this is common practice for DIY
audio HATs but can be jitter-prone at audio rates. The prior research doc
flagged this as "confirm your dtoverlay / board schematic," and that
still applies — worth actually testing before assuming it's fine.

### I2C (control bus)

| Net | ESP32-C6 GPIO | Pi BCM | Pi physical pin |
|---|---|---|---|
| `I2C_SDA` | GPIO22 | **BCM2** | **3** |
| `I2C_SCL` | GPIO23 | **BCM3** | **5** |

Use Pi **I2C1** (pins 3/5) — not the `ID_SD`/`ID_SC` pins (27/28), which
are conventionally reserved for HAT EEPROM identification, not general
I2C.

**Pull-up note:** the Pi has ~1.8kΩ pull-ups to 3.3V built into the Pi
board itself on these two pins, always present when the Pi is powered.
Combined in parallel with this board's own `R11`/`R12` (4.7kΩ, per the
SMD2 design), that's a stronger combined pull-up (~1.3kΩ) when the Pi is
populated vs. 4.7kΩ alone when just the ESP32 is populated. Both are
almost certainly fine at standard/fast I2C timing over short traces, but
worth knowing it changes rather than being surprised by it on a scope.

### SPI / microSD — only if you want ONE shared SD socket

If the goal is a single physical microSD socket usable by whichever
driver is populated (rather than SD only mattering for the ESP32-only
build, per SMD2's existing build-A/build-B split), the SD's SPI signals
need to land on the Pi's fixed SPI0 pins too:

| Function | ESP32-C6 GPIO (per SMD2 build B) | Pi BCM | Pi physical pin |
|---|---|---|---|
| SD CS | GPIO18 | **BCM8** (SPI0_CE0) | **24** |
| SD CLK | GPIO19 | **BCM11** (SPI0_SCLK) | **23** |
| SD MOSI | GPIO20 | **BCM10** (SPI0_MOSI) | **19** |
| SD MISO | GPIO21 | **BCM9** (SPI0_MISO) | **21** |

If SD is only ever going to be used with the ESP32 (as in SMD2's build B,
the Pi being a "dumb driver, no local storage" option), this section
doesn't matter — skip it and just keep SMD2's existing GPIO18-21 XOR with
line-2 SLIC.

### UART — needs a decision, not obviously needed by default

Unlike I2C/I2S/SPI, there's no **shared peripheral on this board** that
needs a UART bus from either driver — nothing here is a UART device. The
ESP32-DevKit already has its own console over its own USB-C (USB-CDC/
JTAG); a Pi typically uses SSH/HDMI or its own separate debug UART setup.
So cross-wiring the Pi's UART0 (BCM14/TXD = pin 8, BCM15/RXD = pin 10) to
the ESP32's UART pins isn't obviously necessary here — **unless** the
actual goal is something more specific, like wanting the ESP32 to remain
present as a co-processor even when the Pi is the main driver (which
would contradict "one or the other"), or wanting a debug link between the
two during bring-up. Worth confirming intent before spending header pins
and routing on this — flagged as **open question** below.

### Power / ground

| Pi pin(s) | Connect to |
|---|---|
| 2, 4 (5V) | Board **+5V** rail (same net as the DC jack / DevKit USB-C input) |
| 1, 17 (3.3V) | Only relevant if borrowing Pi's 3.3V for the codec rail — see [Power](#power) caveat above |
| 6, 9, 14, 20, 25, 30, 34, 39 (GND) | Board **GND** — don't need all of them, but tie a few spread across the header (especially near the I2C/I2S/SPI pin clusters) for clean return current, not just one |

### SLIC control GPIO (PD / SHK / RM / F-R) — no fixed-pin constraint

These are plain digital I/O, not a Pi hardware peripheral — any free
GPIO works on either side. No cross-compatibility requirement here beyond
"pick something free on each driver and land both on the same net,"
unlike I2S/I2C/SPI above.

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

- **UART cross-wiring**: confirm actual intent (see [UART](#uart--needs-a-decision-not-obviously-needed-by-default))
  before routing/spending header pins on it.
- **Shared microSD or ESP32-only SD**: confirms whether the SPI0 section
  above is needed at all.
- **Pi 3.3V budget**: if the Pi-populated case ever needs more than just
  the ES8388 on 3.3V, reconsider a small dedicated LDO for that case
  rather than relying on the Pi's own limited 3.3V rail.
- **DevKit part choice**: specific ESP32-C6-DevKit model/vendor not yet
  pinned down — confirm its onboard regulator's minimum 5V input headroom
  once chosen (affects the DC jack voltage spec above).

## See also

- [`ROTARY_PHONE_SMD.md`](ROTARY_PHONE_SMD.md) — SMD2, still the reference
  for everything not called out as different above (ES8388 pin table,
  AG1171 SLIC/RJ12 wiring, GND plane, JLC assembly notes).
- [`MCU_VS_PI_ZERO_STACK.md`](../MCU_VS_PI_ZERO_STACK.md) — earlier
  either-Pi-or-ESP32 planning exercise (different codec, never built) —
  source of the Pi I2S0 pin mapping used above.
- [`HARDWARE_SIGNAL_LEVELS.md`](../HARDWARE_SIGNAL_LEVELS.md) — SLIC
  levels, SHK dividers.
