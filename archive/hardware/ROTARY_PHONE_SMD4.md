# Rotary Phone Gateway — SMD4 board

Design reference for the **fourth** SMD spin. This doc only covers what's
**different from SMD3** — everything else (pin map, J5 line-1/microSD
jumper mechanism, ESP32-C6 GPIO constraints, PD diode wiring, ES8388/AG1171
wiring, DC-jack diode-OR, JLC assembly notes) still applies unchanged from
[`ROTARY_PHONE_SMD3.md`](ROTARY_PHONE_SMD3.md). **Confirmed via diff**: the
schematic (`Rotary_phone_SMD4.kicad_sch`) is electrically identical to
SMD3's — same nets, same pin assignments, same components. SMD4 is a
from-scratch **2-layer** re-route of that exact design, not a new circuit.

KiCad project: `hardware/kicad/Rotary_phone_SMD4/`.

## Status

Schematic carried over unchanged from SMD3; PCB laid out from scratch as
2 copper layers instead of 4. ERC/DRC reviewed 2026-08-31 — see
[Review findings](#review-findings-2026-08-31).

## What's different from SMD3

| | SMD3 | SMD4 |
|--|------|------|
| Copper layers | 4 (`F.Cu`, `In1.Cu`, `In2.Cu`, `B.Cu`) | **2** (`F.Cu`, `B.Cu` only) |
| Schematic / electrical design | — | **Unchanged** — same pin map, same components, same nets |
| Board thickness | 1.6mm | 1.6mm (unchanged) |

### Why this matters even though "the functionality is the same"

Losing the two inner layers isn't free, even with identical schematics —
worth keeping in mind while routing/reviewing SMD4 specifically, not
because anything's wrong yet, just because these are the standard
2-layer tradeoffs:

- **No dedicated ground/power plane.** SMD3's `In1.Cu`/`In2.Cu` likely
  carried GND (and maybe +5V/+3.3V) as continuous pours, giving every
  signal a low-impedance return path directly underneath it. On 2 layers,
  GND has to be a deliberately-poured zone on F.Cu or B.Cu (whichever
  isn't already packed with signal traces) with stitching vias tying the
  two sides together — worth double-checking there's an actual continuous
  GND pour and not just traces, especially under the audio path (SLIC
  Vin/Vout, ES8388 analog pins) where a broken return path shows up as
  noise/crosstalk, not a dead short.
- **Current-carrying nets** (+5V to the AG1171s' `Vpwr`, in particular)
  don't have a plane to spread heat/drop across — same concern as any
  2-layer board, just worth a trace-width sanity check on that net
  specifically since SMD3 could lean on a plane for it.
- **EMI/crosstalk** is generally worse on 2 layers for the same routing
  density, since there's no plane between signal layers to shield one
  from the other. Not disqualifying for this design (low-speed control
  signals, audio-band I2S, nothing high-speed enough to be fussy) — just
  not a complete wash the way "same schematic" might imply.

None of this showed up as an actual DRC violation (see below) — it's
context for eyeballing the copper pour yourself, not a reported defect.

## Review findings (2026-08-31)

Ran `kicad-cli sch erc` and `kicad-cli pcb drc` against
`hardware/kicad/Rotary_phone_SMD4/`.

**Schematic (ERC):**
- Same 8 PD/EN "not driven" flags as SMD3, same false-positive reasons
  (see [`ROTARY_PHONE_SMD3.md`'s review findings](ROTARY_PHONE_SMD3.md#review-findings-2026-08-30))
  — unchanged schematic, unchanged (non-)issues.
- **20 new flags SMD3 didn't have**: `footprint_link_issues` /
  `lib_symbol_issues` on every JLC-imported part (the diodes, `Card1`/TF-01A,
  the RJ12 jacks) — *"The current configuration does not include the
  footprint/symbol library 'JLCImport'."* This is a **local KiCad library
  resolution difference between the two project copies**, not a schematic
  problem: neither project has its own `fp-lib-table`/`sym-lib-table`, the
  global one on this machine has no `JLCImport` entry either, and a
  leftover `~/Documents/Rotary_phone_SMD2/fp-lib-table` (outside this repo
  entirely, from before the project moved under git) is the only place
  that library is actually registered. SMD3 apparently still resolves it
  through some cached/session state; SMD4, being a fresh copy, doesn't.
  **Worth checking directly in the KiCad GUI** (Preferences → Manage
  Footprint/Symbol Libraries) that `JLCImport` is registered somewhere
  portable (project-local table pointing at a path inside the repo, not
  that leftover home-folder copy) — if it isn't, this design isn't fully
  reproducible from a fresh checkout on another machine, which matters
  more than the ERC noise itself.

**PCB (DRC), 51 flags:**
- Same cosmetic mix as SMD3 (silk-over-copper, silk-overlap, the same
  per-part footprint-library warnings as above, now showing on the DRC
  pass too).
- **No dangling-trace warning this time** — SMD3's one real finding (the
  DC-jack protection diode's `Net-(D1-A)` had a 4.5mm unterminated stub)
  isn't present on this layout. Worth a quick visual confirmation that
  net's fully clean, but DRC + 0 unconnected items on both boards says
  the electrical connectivity itself was never actually broken on SMD3
  either — that finding was stray copper, not a missing connection.

## microSD card-detect (CD) — planned for this board

SMD3/SMD4's schematic currently leaves `Card1` pin 9 (`CD`, the mechanical
card-detect switch — distinct from pin 2's SD-protocol `DAT3/CS`)
unconnected (see [SMD3's review findings](ROTARY_PHONE_SMD3.md#review-findings-2026-08-30)).
Decided 2026-08-31: wire it up on SMD4, using **GPIO3** — currently just
routed to a spare header (net `/Xtra1`, one of two spare switch/LED
headers on this board) with nothing else claiming it.

- **No external resistor needed.** ESP32-C6's internal weak pull-up is
  ~45kΩ (confirmed from `datasheets/ESP32-C6-WROOM-1_datasheet_extracted.txt`'s
  DC characteristics table) — plenty for a slow mechanical switch. Wire
  `Card1` pin 9 straight to GPIO3; firmware enables the internal pull-up.
- Standard push-push socket convention: normally-open, shorts to GND when
  a card is seated. **LOW = card present, HIGH = empty.**
- **Firmware side is already done** (`firmware/components/sdcard/sdcard.c`,
  this session): `sdcard_card_present()` reads a board's `BOARD_SD_CD` pin
  if defined (internal pull-up configured on first use), and
  `sdcard_init()` checks it before touching the SPI bus at all, returning
  `ESP_ERR_NOT_FOUND` with a clear log line instead of attempting (and
  failing) a full mount against an empty slot. Compiles to a no-op
  (`sdcard_card_present()` always returns `true`) on any board that
  doesn't define `BOARD_SD_CD` — SMD2/THT are unaffected.
- **Still needed**: the schematic net itself (GPIO3 ↔ `Card1` pin 9 —
  not yet drawn, this is a plan not a completed wire), and
  `#define BOARD_SD_CD GPIO_NUM_3` in whichever `board_pins_smd4.h`
  eventually gets written (see [SMD3's firmware TODO](ROTARY_PHONE_SMD3.md#firmware-changes-needed-once-the-boards-back),
  which applies here too) — the moment that's added, this feature is live
  with no other code changes.

## SHK divider is marginal as built (found 2026-09-27)

The 10 kΩ / 15 kΩ SHK dividers assume a 5 V SHK swing; the AG1171's SHK is
really ~3.3 V (internal 3.3 kΩ pull-up), so off-hook reads ~1.9 V at the
GPIO -- under the ESP32-C6's 2.475 V guaranteed high. On dualphone2, line 1
(GPIO19) never saw the handset lift even with J5 correctly soldered, while
line 0 (GPIO6) read the same 1.88 V as high. Firmware treats both lines
identically (checked); this is purely which pad happens to switch where.

**Confirmed 2026-09-27:** removing the 15 kΩ on both lines of dualphone2 made both lines detect the handset reliably.

**Rework for built boards:** remove the 15 kΩ on each line (off-hook then
~3.1–3.2 V; the 10 kΩ remains as series protection). **For the next
spin:** 15 kΩ → 100 kΩ. See `docs/hardware-smd4.md`'s rework note.

## Open questions

- **JLCImport library portability** (see above) — confirm it's registered
  via a portable path before this leaves this machine.
- **CD schematic wire** — GPIO3 ↔ `Card1` pin 9 isn't drawn yet, see above.
- Everything else carried over from [`ROTARY_PHONE_SMD3.md`'s open
  questions](ROTARY_PHONE_SMD3.md#open-questions) still applies unchanged
  (J5 jumper hardware choice, factory-reset button on GPIO9, DevKit part
  choice) — this doc doesn't repeat them since nothing here changes the
  answer.

## See also

- [`ROTARY_PHONE_SMD3.md`](ROTARY_PHONE_SMD3.md) — the design this board
  carries over unchanged; pin map, J5 mechanism, GPIO constraints,
  firmware TODO all live there.
- [`smd2.md`](smd2.md) — SMD2, further back (archived: only 3 built, superseded).
- [`smd2.md`'s signal-level rework section](smd2.md#signal-level-rework-history-dtmf-clipping-aec-timing) — SLIC
  levels, SHK dividers.
