# Rotary Phone Gateway — SMD3 board

Design reference for the **third** SMD spin. This doc only covers what's
**different from SMD2** — everything else (ES8388 pin table, AG1171 SLIC
wiring, GND/audio path, JLC assembly notes) still applies from
[`ROTARY_PHONE_SMD.md`](ROTARY_PHONE_SMD.md) unless noted here.

KiCad project: `hardware/kicad/Rotary_phone_SMD3/`.

## Status

Schematic wired and pin-mapped; layout in progress (traces being
permuted/re-routed for board geometry as of 2026-08-30). ERC/DRC reviewed
against `hardware/kicad/Rotary_phone_SMD3/` on that date — see
[Review findings](#review-findings-2026-08-30) for what came out of that
pass. This doc tracks decisions as they're made, not a finished design.

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
for full root-cause detail). **Both confirmed correctly wired in the
SMD3 schematic** (traced through the actual netlist 2026-08-30, not just
eyeballed):

1. **SLIC Vin/Vout swap**: codec `LOUT1`/`ROUT1` (DAC outputs, actively
   driven) now correctly land on each SLIC's **Vin** (needs an external
   driver), and each SLIC's own driven **Vout** feeds the codec's ADC
   input — the reverse of SMD2's bug. Confirmed: `LOUT1`→(C12)→U4.Vin,
   `ROUT1`→(C15)→U3.Vin, U4.Vout→(C13)→R10→codec `LIN1`, U3.Vout→(C14)→R11→codec
   `RIN1`.
2. **Mono ADC input**: each line's mic now lands on its own dedicated
   ES8388 ADC channel instead of sharing one via the `LINSEL` mux — U4
   (line 0) → `LIN1`, U3 (line 1) → `RIN1` (not `LIN1`/`LIN2` as originally
   planned above; using the L/R pair instead of the mux is what actually
   ended up implemented, and it's the correct fix either way). `LIN2`/`RIN2`
   are unused for mic input now.

## Pin map (ESP32-C6-DevKitC-1, U1)

Confirmed from the actual schematic netlist (`kicad-cli sch export netlist`),
not the silkscreen or a guess. **Same net-labeling gotcha SMD2 has**: the
two SLICs are numbered three different ways depending on which net group
you're looking at. The one true mapping:

| Reference | Firmware line | RJ12 jack | Control-signal net prefix | Tip/Ring net prefix |
|--|--|--|--|--|
| U4 | **line 0** | J9 | `/Line0_*` | `/Line1*` |
| U3 | **line 1** | J8 | `/Line1_*` | `/Line2*` |

Yes, U4's *Tip/Ring* nets say "Line1" while its *control* nets say "Line0" —
and U3's say "Line2"/"Line1" respectively. Three numbering conventions,
two physical SLICs. Go by the table above, not by reading a net name in
isolation.

| ESP32-C6 GPIO | Signal | Notes |
|--|--|--|
| GPIO0 | I2S DIN | from codec `ASDOUT` |
| GPIO1 | I2S LRCK | |
| GPIO2 | I2S MCLK | |
| GPIO3 | `/Xtra1` | spare, brought to header J2 pin 1 — unassigned |
| GPIO4 (MTMS, strap) | Line 0 F/R | direct to U4 |
| GPIO5 (MTDI, strap) | Line 0 RM | direct to U4 |
| GPIO6 | Line 0 SHK | via R14/R15 divider, direct to U4 |
| GPIO7 | Line 0 PD | via D3 (diode-only, no pull-up — correct per AG1171 datasheet §3.4, see below), direct to U4 |
| GPIO8 (BOOT, strap) | — | unconnected, free |
| GPIO9 (BOOT, strap) | — | unconnected, free — **this is the download-mode strap**; SMD2 uses it at runtime for the factory-reset-hold button (`board_factory_reset_start()`), SMD3 has nothing wired here yet, see [Open questions](#open-questions) |
| GPIO10 | I2S DOUT | to codec `DSDIN` |
| GPIO11 | I2S BCLK | to codec `SCLK` |
| GPIO12 (USB D-) | — | unconnected, free (native USB-JTAG unused) |
| GPIO13 (USB D+) | — | unconnected, free |
| GPIO14 | — | not brought out on the DevKit's header at all (same module-packaging limit as the WROOM-1) |
| GPIO15 (JTAG, strap) | — | unconnected, free |
| GPIO16 (U0TXD) | header J3 pin 1 | kept as UART/spare per owner's choice — not repurposed |
| GPIO17 (U0RXD) | header J4 pin 1 | same |
| GPIO18 | SD `MISO` **or** Line 1 F/R | shared via jumper header J5, see below |
| GPIO19 | SD `CLK` **or** Line 1 SHK | shared via J5 |
| GPIO20 | SD `MOSI` **or** Line 1 RM | shared via J5 |
| GPIO21 | SD `CS` **or** Line 1 PD | shared via J5 |
| GPIO22 | I2C SDA | codec control |
| GPIO23 | I2C SCL | codec control |
| EN/RESET | — | not connected on the carrier board — the DevKitC-1 module already has its own pull-up + reset button onboard, nothing to add here |
| GPIO24-30 | — | not applicable — internal flash, not part of this module's usable pin set |

### J5: line 1 SLIC vs. microSD, jumper-selected

Unlike SMD2 (which XORs the same *physical pads* between line 2's SLIC and
a microSD via a build-time choice), SMD3 gives GPIO18-21 to the microSD
socket directly and brings line 1's SLIC control signals (F/R, RM, PD,
SHK) out to the *other* pin of each column-pair on header **J5** (2x4,
0.1"). **Confirmed design intent**: J5 takes 2-pin jumper shunts across
each pair to route those four GPIOs to line 1's SLIC instead of the SD
card when you want two lines instead of storage — same "one set of copper,
pick one peripheral" idea as SMD2, just resolved with a physical jumper
instead of firmware Kconfig.

| J5 pin | Signal | J5 pin | Signal |
|--|--|--|--|
| 1 | GPIO21 (SD CS) | 2 | Line 1 PD |
| 3 | GPIO20 (SD MOSI) | 4 | Line 1 RM |
| 5 | GPIO19 (SD CLK) | 6 | Line 1 SHK |
| 7 | GPIO18 (SD MISO) | 8 | Line 1 F/R |

Bridge 1-2, 3-4, 5-6, 7-8 for line 1; leave all four open (or don't stuff
the jumper block) to use the microSD instead. **Without a jumper block
populated, line 1 has no path to any GPIO at all** — worth deciding now
whether J5 gets an actual jumper header/shunts or just solder-jumper pads,
since "populate 4 loose jumper wires" isn't a great out-of-box experience.

### ESP32-C6 GPIO constraints (reference, so this doesn't need re-deriving)

From ESP-IDF's own C6 GPIO table (`docs/en/api-reference/peripherals/gpio/esp32c6.inc`),
not guesswork:

- **Strapping pins: GPIO4, 5, 8, 9, 15.** Fine to use as general I/O after
  boot; what's wired to them just can't force the wrong level *during*
  power-on/reset. GPIO9 is the one that actually matters (download-mode
  select) — everything else is lower-stakes. SMD3 already proves strapping
  pins are fine for peripheral-driven signals here: GPIO4/5 drive the SLIC
  (ESP32 output → external chip), same direction I2S BCLK/LRCK ran on
  SMD2's GPIO4/5 without issue.
- **GPIO12/13**: USB-Serial-JTAG by default, but not a boot hazard — just
  don't enable that peripheral and they're plain GPIOs.
- **GPIO24-30**: internal flash, never available.
- **GPIO14**: not led out on WROOM-1-based modules (module packaging, not
  a chip limitation).

## Review findings (2026-08-30)

Ran `kicad-cli sch erc` and `kicad-cli pcb drc` against the schematic/PCB
after the pin permutation + re-route. Full output not preserved here, just
what mattered:

**Schematic (ERC), 8 flags, all false positives or confirmed-fine:**
- 4x "SLIC PD not driven by an output" — expected. AG1171 datasheet §3.4:
  *"The PD input should not be taken to a logic (H)... Connecting to PD
  via a diode will ensure it is never taken (H)."* D2/D3's diode-only (no
  pull-up) wiring is exactly that recommendation; ERC just can't reason
  through a diode.
- `EN/RESET` "not driven" — fine, the DevKitC-1 module has its own onboard
  pull-up + reset button.
- microSD `CD` (pin 9, the mechanical card-detect switch — distinct from
  pin 2's `DAT3/CS`) unconnected — **left unconnected on purpose**: this
  project has never used hardware card-detect, `sdcard_init()` handles a
  missing/removed card via mount-attempt failure already. If that ever
  changes, pin 9 needs an external pull-up and a non-strapping GPIO
  (GPIO12/13 are free) — a card seated in the slot at power-on would hold
  a strapping pin low through boot otherwise.

**PCB (DRC), 51 flags, one real:**
- 29 silk-over-copper + 6 silk-overlap + 15 footprint-library warnings —
  cosmetic/library housekeeping (custom `Scott:` and JLC-imported
  footprints reading as "modified" against the library table), not
  electrical.
- **One dangling trace**: `Net-(D1-A)` (DC jack → protection diode D1,
  the power-input diode-OR path) has an unconnected end, 4.5mm long, at
  board position (94.0, 93.0mm). The schematic-level connection is
  correct (verified: D1 anode ← J1 `PWR`, cathode → `+5V`) — the copper
  on the PCB just doesn't actually close the loop at that spot yet.
  **Needs fixing before this is orderable.**

## Firmware changes needed once the board's back

None of this is implemented yet — `board_pins_smd.h` is SMD2-only today
(its own header says so). SMD3 needs its own pin map, not an edit to that
file, since almost nothing lines up:

- New `board_pins_smd3.h` (or rename the existing one to make room) with
  the GPIO table above. `board_line_pins()`'s line-0/line-1 split still
  works the same way — line 0 is still "the always-direct one," it's just
  on GPIO4/5/6/7 instead of SMD2's GPIO1/2/3/10.
- New `CONFIG_BOARD_TYPE_SMD3` Kconfig choice alongside the existing
  `_THT`/`_SMD2` ones (`firmware/components/board/Kconfig`), wired through
  wherever those two are currently switched (`board_config.h`, codec
  driver selection — SMD3 still uses ES8388 like SMD2, so that part of the
  dispatch shouldn't need new logic, just the new pin header).
- PD logic itself needs no change — firmware already drives it as a
  plain GPIO output (`BOARD_PD_LEVEL_RUN=0`/`POWERDOWN=1`), and that's
  compatible with the diode: driving low pulls PD low (down through the
  diode, power-down engaged); driving high lets the diode block and PD
  floats open, which the datasheet treats as equivalent to "not low" (run).
- If line 1's J5 jumpers are populated, firmware treats it exactly like
  SMD2's line 2 — same `board_hw_config_get()`/`board_hw_config_set()`
  runtime choice between "SD card" and "second line," just gated on a
  physical jumper instead of GPIO18-21 being wired to one peripheral or
  the other at fab time. Worth deciding whether `board_hw_detect_evidence()`'s
  passive-read heuristic (used on SMD2 to guess which peripheral is
  actually populated) still makes sense here, or whether SMD3 should just
  always ask the human, since a jumper is easier to get wrong silently
  than a populate-or-don't BOM choice.

## Open questions

- **DevKit part choice**: specific ESP32-C6-DevKit model/vendor not yet
  pinned down — confirm its onboard regulator's minimum 5V input headroom
  once chosen (affects the DC jack voltage spec above).
- **Dangling DC-jack trace** (`Net-(D1-A)`, see [Review findings](#review-findings-2026-08-30)) —
  needs closing before this is orderable.
- **J5 jumper hardware**: an actual 2x4 jumper header + 4 shunts, or just
  bare pads meant for solder-bridges/wire jumpers? Affects BOM and how
  approachable "switch to line 1" is for a future build.
- **Factory-reset button on GPIO9**: SMD2/THT firmware reads GPIO9 at
  runtime for the hold-to-factory-reset gesture; SMD3's GPIO9 is currently
  unwired. Decide whether that feature carries over to SMD3 (needs a
  button + pull-up on GPIO9, careful not to load it at power-on) or gets
  dropped for this board.

## See also

- [`ROTARY_PHONE_SMD.md`](ROTARY_PHONE_SMD.md) — SMD2, still the reference
  for everything not called out as different above (ES8388 pin table,
  AG1171 SLIC/RJ12 wiring, GND plane, JLC assembly notes).
- [`HARDWARE_SIGNAL_LEVELS.md`](../HARDWARE_SIGNAL_LEVELS.md) — SLIC
  levels, SHK dividers.
