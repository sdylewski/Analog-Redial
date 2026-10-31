# Coin board: through-hole vs. surface-mount BOM

Companion to [coin-board-schematic.md](coin-board-schematic.md) and
[output-stage-wiring.md](output-stage-wiring.md). 2026-10-30. Plan: build v1
through-hole (easy to probe and rework), and keep the **same schematic** so an
SMT spin only changes footprints (and, for a few transistors, the symbol).

Only the parts that matter are listed: ordinary resistors and small
ceramics are left out unless they need a power or voltage rating. LCSC
numbers marked **(verify)** are from memory or common use, not checked
against JLCPCB today; JLCPCB's Basic list is paginated (1,586 items) and the
part pages don't say Basic or Extended, so check each in the BOM upload.

## How the SMT spin would work in KiCad

- Keep the one schematic. For the SMT board, copy the project and change
  footprints (Tools -> Assign Footprints does it in one table).
- **Transistors need their symbol swapped too**, because KiCad numbers the
  pins differently by package: `2N7000` (TO-92) is 1 S, 2 G, 3 D, while
  `2N7002` (SOT-23) is 1 G, 2 S, 3 D. Swap the symbol (same nets), don't just
  change the footprint, or the board will be wired wrong. Same for
  2N3904/2N3906 (TO-92, E-B-C) vs MMBT3904/MMBT3906 (SOT-23, B-E-C).
- The two DIP-8 chips keep their pinout in SOIC-8, so their values and
  wiring stay exactly the same.

## BOM: THT (v1) vs SMT, with prices

DigiKey qty-1 prices looked up 2026-10-30 (the cheapest active listing
shown; quantity breaks drop most of them). LCSC/JLCPCB prices weren't
readable from their pages, but LCSC is usually at or below DigiKey for these.
"Basic"/"Extended" is my expectation, not confirmed (JLCPCB's pages don't
show it): check in the BOM upload.

| Ref | Function | **THT part** | THT $ | **SMT part** | SMT $ | JLCPCB | Recommendation (5-board order) |
|---|---|---|---:|---|---:|---|---|
| U1 | I2C I/O expander | MCP23008-E/P, DIP-18 | 1.54 | MCP23008-E/SO, SOIC-18 | 1.37 | Extended | hand-solder (SOIC is easy) |
| U2 | stage 1, 5 V to 20 V | MC34063AP1G, DIP-8 | 1.98 | MC34063ADR2G, SOIC-8 | 0.77 | Extended | hand-solder |
| U3 | stage 2 controller | **UC3843AN (TI)**, DIP-8 | **1.79** | **UC3843BD1R2G**, SOIC-8 (LCSC C16414) | 0.71 | Extended | hand-solder, or JLCPCB places it from LCSC stock. DigiKey US stock was thin (56) on 2026-10-30 |
| Qa | stage 2 switch, 200 V | IRF630PBF, TO-220 | 2.43 | keep TO-220 (or a 200 V DPAK) | 2.43 | n/a | hand-solder, through-hole |
| Q3 | fire switch, 200 V | IRF630PBF, TO-220 | 2.43 | STN4NF20L, SOT-223 | 1.02 | Extended | hand-solder |
| Q2, Q4, Q5 | small N-FETs (x3) | 2N7000, TO-92 | 0.29 | 2N7002, SOT-23 | 0.02-0.26 | Basic (C8545, verify) | **JLCPCB places** (3 per board, no fee) |
| Q6 | gate-drive PNP | 2N3906, TO-92 | 0.28 | MMBT3906, SOT-23 | 0.15 | Basic (verify) | **JLCPCB places** |
| D1 | stage 1 Schottky | 1N5819, DO-41 | 0.32 | SS34, SMA | 0.22 | Basic (C8678, on v6) | **JLCPCB places** |
| D2 | stage 2 ultrafast | UF4003, DO-41 | 0.64 | ES2D, SMA | 0.40 | Extended | hand-solder |
| D3 | HV clamp | 1.5KE150A, DO-201 | 0.19 | SMBJ130A, SMB | 0.51 | Extended | hand-solder (or keep the 1.5KE through-hole) |
| D4 | relay flyback | 1N4148, DO-35 | ~0.10 | 1N4148W, SOD-123 | ~0.05 | Basic (C81598, on v6) | **JLCPCB places** |
| D6, Dz | gate zeners 12 V, 15 V | 1N4742A / 1N4744A, DO-41 | 0.05-0.28 | BZT52C12 / C15, SOD-123 | 0.01-0.14 | likely Basic (verify) | **JLCPCB places** if Basic, else hand-solder |
| C1 | 220 uF / 200 V | UCS2D221MHD, radial | 3.08 | (none) | | n/a | through-hole on both boards |
| L1 | 100 uH, 2 A | **Würth [7447471101](https://www.digikey.com/en/products/detail/w%C3%BCrth-elektronik/7447471101/3476776)** (WE-TIS, shielded, 11 mm, 5 mm pitch; 1.4 A rated, 2.1 A Isat, 0.24 ohm) | 2.49 | Bourns SRR1260-101M, 12 x 12 mm | ~1.05 (est.) | Extended | **through-hole on the SMT board too** |
| L2 | 220 uH, 1 A | **Würth [7447471221](https://www.digikey.com/en/products/detail/w%C3%BCrth-elektronik/7447471221/3476777)** (WE-TIS, shielded, 11 mm, 5 mm pitch; 1.0 A rated, 1.4 A Isat, 0.51 ohm) | 2.49 | Bourns SRR1260-221K / Murata 48221SC | 1.05 / 1.14 | Extended | **through-hole on the SMT board too** |
| K1 | DPDT relay | G6S-2-DC5 | 4.52 | G6S-2F-DC5 | 5.08 | Extended | keep through-hole (cheaper, sturdier) |
| R, C | 0805 resistors/caps | axial / disc | | 0805 | | Basic for common values | **JLCPCB places** |
| R11, Rcs, R26 | power resistors | axial | | 1206 / 2512 | | check | Basic values exist for some |
| C5, C7, C9, C14, C15 | 5 V input and 20 V rail bulk | 22 uF 35 V electrolytic, radial 5 mm, 2 mm pitch | | 22 uF 35 V SMD electrolytic, 6.3 x 5.4 mm can | | Extended (no Basic one found) | **through-hole on the SMT board too** (SMD options below) |
| J1-J3, J6-J9, RV1 | connectors, trimmer | | | stay through-hole | | n/a | hand-solder |
| U4 | ADS1115 (debug only) | AliExpress module, ~1.90 ([listing](https://www.aliexpress.us/item/2251832488241448.html)) | 1.90 | ADS1115IDGSR, VSSOP-10 | 5.28 | Extended | leave unfitted; module for bring-up |

**Per board, the semiconductors** (U1-U3, Q2-Q6, D1-D6) come to about
**$11 through-hole vs. about $5 SMT** at these prices.

### UC3843 in surface-mount: what else fits

The 8-pin UC3842/3/4/5 family all share one pinout, and the "43" is the one with
the 8.4 V start / 7.6 V stop threshold this board needs. DigiKey, 2026-10-30:

| Part | Maker | Package | $ (qty 1) | Stock | Drop-in? |
|---|---|---|---:|---:|---|
| **UC3843BD1R2G** | onsemi | SOIC-8 | 0.71 | 56 | **yes**: the same bipolar UC3843 (30 V supply rating), same R/C values. LCSC C16414 |
| UC3843BD1 | ST | SOIC-8 | | | yes, ST's version of the same part |
| UCC38C43DR, UCC28C43DR, -DGKR | TI | SOIC-8 / VSSOP-8 | 0.90-2.52 | | **no on this board**: the BiCMOS UCC28C4x/38C4x are rated **20 V absolute maximum, 18 V recommended** for VDD ([TI datasheet SLUS458](https://www.ti.com/document-viewer/UCC38C43/datasheet/GUID-7D2F25E7-C9BF-401E-BD1E-0AAA9B0B1602)), and U3's VCC is the 20 V rail |
| UC3843AD | TI | **14-pin SOIC** | 2.49 | | **not a drop-in**: TI's "D" UC3843A is the 14-pin version with a different pinout |

The SMT board uses the **UC3843BD1R2G**. To use a UCC38C43 anyway, stage 1 would have to make 15-16 V
instead of 20 V (R13/R14), which also lowers the gate drive.

Not drop-ins: the **UC3842** (16 V start / 10 V stop, too close to the 20 V
rail) and the **UC3844/UC3845** (their output is limited to 50 % duty, too low
for a 6.5:1 boost).

### Is the $3 Extended fee worth it?

The fee is **per unique Extended part, per order**, not per board, so it
shrinks with order size:

| Boards in the order | Fee per Extended part, per board |
|---:|---:|
| 5 | $0.60 |
| 10 | $0.30 |
| 30 | $0.10 |

With about 6 Extended SMT parts here (U1, U2, U3, Q3, D2, D3; L1 and L2 are
through-hole), that's **$18 per order**: $3.60 a board at 5 boards, $0.60 at 30.

**Recommendation**

- **First SMT order (5 boards): JLCPCB places only Basic parts**, which are free
  to place: the 2N7002s, MMBT3906, SS34, 1N4148W, zeners, and the 0805 R/C.
  You hand-solder the Extended ones. They're all easy packages (SOIC-8/18,
  SOT-223, SMA, SMB), only one of each per board, and
  hand-soldering them saves about $18 an order, nearly the whole saving from
  going SMT.
- **30 boards or more: let JLCPCB place everything SMT.** The fees drop to
  about $0.60 a board, less than your time.
- **Through-hole on every version:** C1, K1, Qa (TO-220), L1/L2 (Würth
  WE-TIS, shielded), the five 22 uF electrolytics, the connectors and
  jumpers. JLCPCB's through-hole service costs extra too, and
  these solder in a minute.
- Buy the hand-solder parts from LCSC in the same order (they ship with the
  boards) or from DigiKey.

## Will small resistors and capacitors stand the voltage?

**Resistors.** Thick-film chips have a maximum working voltage, separate from
their power rating. Typical figures (UniOhm/Yageo-class general parts; check the
datasheet of the exact part):

| Size | Max working voltage (typ.) | Power (typ.) |
|---|---|---|
| 0402 | 50 V | 1/16 W |
| 0603 | 50-75 V | 1/10 W |
| 0805 | 150 V | 1/8 W |
| 1206 | 200 V | 1/4 W |
| 2512 | 200 V | 1 W |

Where it matters on this board:
- **The HV chains** (bleed, feedback top, ADC divider) are 3 in series, so at
  about 120-130 V each resistor sees about 40-45 V. That's within 0603 on
  paper but with little margin, and 0603 pads sit only about 0.8 mm apart.
  **Use 1206** (or at least 0805) for anything on a chain from HV.
- Everything else (logic, 20 V, gate drive) is fine in 0805, or 0603.

**Ceramics on the 20 V rail need 50 V parts:** Cvcc1 (U3's VCC bypass) is
marked **0.1uF 50V** in the schematic; a 16 or 25 V 0805 is too close to 20 V
and loses most of its capacitance there.

**Capacitors.** There are no ceramic capacitors directly on HV in this design
(C1 is the only HV capacitor and it stays through-hole). For the rest, pick a
rated voltage at least 2x the working voltage, and mind DC bias:

- **22 uF at 20 V:** a 22 uF 25 V 0805 X5R (C45783) loses most of its
  capacitance at 20 V (often 70-80 % gone from DC bias), and 20 V on a 25 V
  part is too close. On the 20 V rail use **50 V 1206** parts (22 uF if
  stocked, or 2 x 10 uF), or keep an SMD electrolytic.
- **0805 for rework:** yes, 0805 is a good choice for a board you may want to
  modify; 0603 is noticeably fiddlier with an iron. Use 1206 for the HV chains
  and the 1/4-1/2 W resistors.

## The 22 uF electrolytics (C5, C7, C9, C14, C15)

Through-hole radial parts (5 mm, 2 mm pitch, `CP_Radial_D5.0mm_P2.00mm`) on
this board. For a later spin with JLCPCB placing them: none of the 22 uF 35 V
SMD electrolytics found at JLCPCB are Basic, but all five are the same part,
so it's a single $3 Extended fee per order, not five. SMD parts that fit
`Capacitor_SMD:CP_Elec_6.3x5.4` (LCSC numbers; check
stock and Basic/Extended in the BOM upload):

| Part | Maker | LCSC | Can | Note |
|---|---|---|---|---|
| CS1V220M-CRE54 | ST (Semtech) | [C116086](https://datasheet.lcsc.com/lcsc/1811082138_ST-Semtech-CS1V220M-CRE54_C116086.pdf) | 6.3 x 5.4 mm | exact size |
| VES220M1VTR-0605 | Lelon | [C249703](https://www.lcsc.com/product-detail/C249703.html) | 6.3 x 5.3 mm | fits the same pads |
| AVEZ226M35D16T-F | Cornell Dubilier | [C2443005](https://jlcpcb.com/partdetail/2534856-AVEZ226M35D16TF/C2443005) | 6.3 x 5.3 mm | fits the same pads |

Not these: CS1V220M-CRD54 (C116404) is the 5 x 5.4 mm can, too small for
this footprint.

## Basic vs. Extended at JLCPCB

- **22 uF:** C45783 (Samsung CL21A226MAQNNNE, 22 uF 25 V X5R 0805) is the
  usual Basic 22 uF; I confirmed the part, not its Basic status. It's right
  for the 5 V side, not the 20 V rail.
- **220 uF:** I didn't find a Basic 220 uF in what I could read, and the one
  this board needs (200 V) isn't an SMD part anyway; it stays through-hole.
- Most of the special parts here (MCP23008, MC34063, UC3843, the 200 V FETs,
  SMBJ130A, the relay, the inductors) will be **Extended**. JLCPCB charges a
  per-part setup fee for each Extended part (about $3 each last I knew;
  check the current figure), so 8-10 of them add roughly $25-30 per order,
  on top of the part prices. Through-hole parts JLCPCB doesn't place stay
  hand-soldered either way.
- Your v6 board already uses several Basic parts this board can share:
  SS34 C8678, 1N4148W C81598, and the 0402 R/C values (here in 0805 instead).

## PCB layout and routing

The parts in `Payphone_v1.kicad_pcb` are placed in blocks, each a KiCad
**group** (click one part and the whole block selects; drag it as a unit),
with room left inside each block for routing. J1, J2, J3 and J6 stay where
they were, locked. There is no board outline yet: draw Edge.Cuts once the
mounting is decided.

On the board:

- **F.Silkscreen** (fabricated): each block's name, the HV warning by C1 ("stays charged
  until the HV LED is out"), and the J7-J9 voltage table.
- **User.Comments** (not fabricated): a dashed outline around each block and
  the routing notes below, next to the block they're about. Hide the layer
  when it gets in the way.

| Block (group) | Parts | Sits |
|---|---|---|
| LOGIC | U1, C16, R10, U4, C17, R17, R19, R33 | top left, beside J6/J1 |
| (not placed yet) | JP1; debug LEDs D8-D13 with R34-R39; HV LED R27, R28, D5, D7 | see "Still to do" |
| COIN | R4-R9, C2-C4 | top right, at J3 |
| STAGE1 (5 V -> 20 V) | U2, L1, D1, R11-R14, Q1, R1, C5-C9, C14, C15 | bottom left |
| STAGE2 (20 V -> HV, with C1) | U3 and its small parts, Qa1, L2, D2, Rcs1, Dz1, Rg1, J7-J9, Rfbb1-4, Rfbt1-3, C1, D3, R32, Rbl1-3 | bottom middle |
| FIRE | R3, R21-R25, Q4, Q5, D6, Q3, R26 | right, above K1 |
| POL | K1, Q2, D4, R2, R20 | right, beside J2 |

Power flows left to right: J6 -> stage 1 -> stage 2 -> C1 -> K1 -> J2.

### Ground

- **One ground, no splits.** There's no transformer, so the "HV ground" and
  the logic ground are the same node, and FIRE, POL, I2C and the ADC lines
  all cross between blocks: a split would make their return currents detour.
- Bottom layer: **one solid GND pour**, with only short bottom-layer jumpers,
  so the pour isn't cut into islands.
- Close each power loop on the top layer, then stitch its ground to the pour
  with several vias at its capacitors. Placement keeps the power currents out
  of the logic block; the plane doesn't need cutting for that.
- A GND pour under HV traces on the other layer is fine (1.6 mm of FR4). On the
  same layer, the pour keeps the HV net class's 1.0 mm: check the zone's
  clearance follows the net class.

### Stage 1 (MC34063)

- **Hot loop:** U2 pin 1 (SwC) -> D1 -> C7/C9 -> GND -> U2 pin 2. Smallest
  possible, all on top.
- **Input:** J6 +5V -> C5/C6 -> R11 -> U2 pin 7 and L1. Wide traces (1 mm or
  more): this draws about 1 A peaks from the main board.
- **SW1** (L1, D1 anode, U2 pin 1): short, with little copper.
- C8 at pin 3; R13/R14 at the FB pin (pin 5).
- 20V leaves from C9/C15 to stage 2 (L2, U3 pin 7), and up to Q5/R23 in the
  FIRE block.

### Stage 2 (UC3843)

- **Hot loop:** Qa1 drain -> D2 -> C1+ ... C1- -> Rcs1 -> Qa1 source.
  C1 is large, so this loop is too. A **100 nF 250 V ceramic** (1206 X7R)
  from D2's cathode to Rcs1's GND end would close the fast part of it; it
  isn't in the schematic yet.
- **SW2** (Qa1 drain, L2, D2 anode) swings 0-120 V at the switching
  frequency: short, and nothing parallel to it. Qa1's tab is SW2 too, so keep
  its copper moderate.
- **Gate:** U3 OUT -> Rg1 -> Qa1 gate, returning through Qa1 source / Rcs1 to
  U3's GND. Dz1 right at the gate and source.
- **Current sense:** a separate (Kelvin) trace from Rcs1's top pad to Rf1;
  Cf1 at U3's CS pin.
- **Quiet ground:** Ct1, Cvref1, Cf1, Rfbb4 and J7-J9 go to U3 pin 5 (GND),
  then one via to the plane. RT/CT noise is what upsets UC384x designs.
- **Feedback:** the FB node is short, with Rfbb1-4, Rz1 and Rfbt3 at U3 pin 2.
  HV runs out to Rfbt1; the high-impedance FB node doesn't run out to HV.

### HV

- HV, HV_*, SW2, COIL_A/B and RETURN are in the **HV net class** (1.0 mm
  clearance), so DRC enforces it.
- D3 straight across C1; Rbl1-3 beside it.
- **R32 sits at C1:** only its 3 V tap (AIN0) runs to U4, so no HV enters the
  logic block.
- Silkscreen: the HV warning (C1 stays charged until D7, the HV LED, is out)
  and the J7-J9 table are on the board.

### FIRE and POL

- FIRE and POL leave U1 as plain VIO logic lines. **Long runs are fine**; the
  level shift happens at the far end, so GATE3 (R24/R25/D6 -> Q3 gate) is only
  a few millimetres. Keep these lines away from SW1 and SW2; crossing them at
  90 degrees on the other layer is fine.
- **Firing pulse loop:** C1+ -> K1 -> J2 -> coin relay -> K1 -> Q3 -> R26 ->
  GND -> C1-. Keep it compact, with R26's GND via toward C1-.
- **K1:** its HV (3, 8) and RETURN (5, 10) pads cross over, so route one of
  the pairs on the bottom layer. Q2 and D4 sit at the coil pins (1, 12).

### ADC lines

- Each divider's low end sits at U4 (R33 for AIN0, R19/R17 for AIN1), so the
  long runs are the 3 V taps.
- **Add 10-100 nF from each AIN pin to GND** at U4 (not in the schematic yet):
  the taps are about 30 k source impedance, and the cap makes them
  low-impedance to noise. The readings are slow, so the delay costs nothing.
  The same for AIN2 (ISENSE from R26).

### Still to do on the board

- **New parts:** run Update PCB from Schematic (it also renames +3V3 to VIO
  and changes Rbl1-3 to 1 M), then place:
  - **HV LED** (R27, R28, D5, D7): in the STAGE2 group beside C1, with D7
    where it can be seen with the board installed. R27/R28 are HV class.
  - **Debug LEDs** D8-D13 with R34-R39: in a row along an edge, labelled on
    silkscreen 5c / 10c / 25c / HV EN / POL / FIRE. D8-D10 and R34-R36 near
    J3 (COIN group), D11-D13 and R37-R39 near U1 (LOGIC group).
  - **JP1** (VIO = 5V) near J6, with its label on silkscreen.
  - JLCPCB parts, checked 2026-10-31 on the part pages: red 0805 LED
    **C84256** (Basic; no 1206 red LED is Basic, so 0805 is the largest),
    green 0805 LED **C2297** KT-0805G (Basic; the 0603 greens C72043 and
    C12624 are Extended). Still to check: BZT52C22 in SOD-123, 33 k / 1 M
    1206, 1.5 k 0805.
- **Pad-to-pad clearance inside footprints:** with the HV class's 1.0 mm,
  DRC flags the pin spacing of K1 (0.54 mm between neighbouring pads), J2
  (0.8 mm), Qa1 (0.64 mm) and Q3 (0.8 mm). These are the parts' own pitches.
  Either add a rule that relaxes clearance between pads of the same footprint,
  or exclude them in DRC. K1's pad 3 (HV) to pad 4 (COIL_A) at 0.54 mm is under
  IPC-2221's 0.6 mm for bare external copper at 101-150 V; smaller pads on
  K1's contact pins would bring it over.
- Board outline and mounting holes (M3, away from HV).
- Test points: 20 V, VIO, HV (a probe pad behind a series resistor), GND.
- The AIN filter caps and the 100 nF 250 V stage-2 cap, if you want them:
  add them to the schematic first, then Update PCB.
- Two reference designators overlap (Cz1/Rz1, Ct1/R18): nudge them when
  tidying silkscreen.
