# Coin board: parts and PCB layout

**In progress, not finished:** the PCB isn't routed yet.

Companion to [coin-board-schematic.md](coin-board-schematic.md) and
[output-stage-wiring.md](output-stage-wiring.md). Reference designators and
values match `Payphone_v1/Payphone_v1.kicad_sch`.

The board is surface-mount, with through-hole only where it's the better part:
C1, K1, Qa1, U4's module and the connectors. None of them has an LCSC number, so
none lands on the JLCPCB BOM and there's no hand-solder fee.

## Parts

DigiKey qty-1 prices looked up 2026-10-30, unless marked; quantity breaks drop most
of them, and LCSC is usually at or below DigiKey. "Basic" is confirmed only where
marked; **(verify)** means my expectation, from memory or common use, so check it in
the BOM upload.

**Surface-mount semiconductors**

| Ref | Function | Part | $ | LCSC / JLCPCB | Fit |
|---|---|---|---:|---|---|
| U1 | I2C I/O expander | TCA9534APWR, TSSOP-16, 0.65 mm | ~0.45 (LCSC) | C206010, Extended | JLCPCB, or hand-solder |
| U2 | stage 1, 5 V to 20 V | MC34063ADR2G, SOIC-8 | 0.77 | Extended | hand-solder |
| U3 | stage 2 controller | UC3843BD1R2G, SOIC-8 | 0.71 | C16414, Extended | hand-solder, or JLCPCB. DigiKey US stock was thin (56) |
| Q3 | fire switch, 200 V | STN4NF20L, SOT-223 | 1.02 | Extended | hand-solder |
| Q1, Q4 | small N-FETs | 2N7002, SOT-23 | 0.02-0.26 | C8545, Basic (verify) | **JLCPCB** |
| Q2 | relay driver NPN | MMBT3904, SOT-23 | | Basic (verify) | **JLCPCB** |
| Q5 | gate-drive PNP | MMBT3906, SOT-23 | 0.15 | C7420354, Preferred (no Extended fee) | **JLCPCB** |
| D1 | stage 1 Schottky | 1N5819HW, SOD-123 | | (verify) | **JLCPCB** if Basic. SS34 (C8678, Basic, on the v6 board) is the alternative, on an SMA footprint |
| D2, D14 | stage 2 ultrafast, coil freewheel | ES2D, SMA | 0.40 | Extended | hand-solder |
| D3 | HV clamp | SMBJ130A, SMB | 0.51 | Extended | hand-solder |
| D4 | relay flyback | 1N4148W, SOD-123 | ~0.05 | C81598, Basic (on v6) | **JLCPCB** |
| D5, D6 | zeners, 22 V, 12 V | BZT52C22, BZT52C12, SOD-123 | 0.01-0.14 | likely Basic (verify) | **JLCPCB** if Basic |
| Dz1 | Qa1 gate zener, 15 V | SMAZ15, SMA, 1 W | | (verify) | **JLCPCB** if Basic |
| D7 | HV indicator | red LED, 0805 | | C84256, Basic | **JLCPCB** |
| D8-D13 | debug LEDs | green LED, 0805, KT-0805G | | C2297, Basic | **JLCPCB** |

The Basic LEDs were checked on JLCPCB's part pages on 2026-10-31. No 1206 red LED is
Basic, so 0805 is the largest for D7; the 0603 greens (C72043, C12624) are Extended.

**Surface-mount inductors**

Both are SXN's SMDRI127 (12.3 x 12.3 x 8 mm, shielded), on KiCad's
`Inductor_SMD:L_SXN_SMDRI127` footprint. JLCPCB prices and stock checked 2026-11-03.
SXN rates saturation at a 25 % drop in inductance.

| Ref | Value | Part | Isat | Heating (40 C rise) | DCR max | Peak here | $ (JLCPCB, qty 1) | LCSC / JLCPCB | Fit |
|---|---|---|---|---|---|---|---:|---|---|
| L1 | 100 uH | SMDRI127-101MT | 3.5 A | 1.7 A | 0.22 ohm | about 1 A (R11/R45/R46) | 0.23 | C10150, Extended, 64,000 in stock | **JLCPCB** |
| L2 | 220 uH | SMDRI127-221MT | 2.0 A | 1.16 A | 0.39 ohm | 0.45 A (Rcs1) | 0.23 | C10151, Extended, 17,000 in stock | **JLCPCB** |

Each is an Extended part, so JLCPCB adds its per-part fee (about $3 per order) on top.

**Surface-mount electrolytic and fuse** (JLCPCB, Extended, checked 2026-11-04)

| Ref | Function | Part | $ | LCSC | Why this one |
|---|---|---|---:|---|---|
| C5, C7, C14, C20 | 100 uF / 50 V on +5V and 20V | Ymin VKMD1001H101MV, 8 x 10 mm, footprint `CP_Elec_8x10` | 0.24 | C487360 | 724 mA ripple at 100 kHz against about 0.25 A in C7 and 0.15 A in C14; 10,000 h at 105 C. Aluminium, not polymer: C20 needs some ESR to damp the cable. One part for all four |
| F1 | resettable fuse on +5V | Littelfuse 1812L110PR, 1812 | 0.05 | C163516 | 1.1 A hold, 2.2 A trip, 8 V (the same hold and trip as the MF-R110 it replaces) |

Ceramics don't suit the 100 uF positions: a 22 uF 35 V X5R keeps only about a third of its
capacitance at 20 V, and has too little ESR for C20.

**Through-hole, hand-soldered ("do not place" for JLCPCB)**

| Ref | Function | Part | $ |
|---|---|---|---:|
| C1 | 220 uF / 200 V | Nichicon UCS2D221MHD, 18 x 26.5 mm, 7.5 mm leads | 3.08 |
| K1 | DPDT relay, 5 V coil | Omron (Aratas) G6S-2-DC5: 250 VAC / 220 VDC switching per the listing **(check the datasheet)** | 4.52 |
| Qa1 | stage 2 switch, 200 V | IRF630PBF, TO-220 | 2.43 |
| J2 | relay cable | Molex 0026604030, KK 396, 3-pin (plug 09-50-3031 + terminals, verify) | 0.35 |
| J3 | coin switch cable | Molex 0026604040, KK 396, 4-pin (plug 09-50-3041 + terminals, verify) | 0.44 |
| J1 or J6 | main board cable | JST B5B-EH-A, or a 1 x 5 pin header, 2.54 mm | |
| J7-J9 | HV setting | 1 x 2 pin headers + shunts, 2.54 mm | |
| U4 | ADC | ADS1115 module on a 1 x 10 header, 2.54 mm (VDD, GND, SCL, SDA, ADDR, ALRT, A0-A3) | under 2 |
| J5, R42, R43, Rslope1, Cslope1 | spare I/O pads, I2C pull-ups, slope compensation | **not fitted** | |

These come to about **$16.50** a board at DigiKey qty-1 prices, U4's module included (section "What it costs").

**Resistors and small capacitors: 0805, JLCPCB places them**, except:

| Ref | Value | Size | Why |
|---|---|---|---|
| Rfbt1-3, Rbl1-3, R27, R28, R32 | 100 k, 1 M, 33 k, 1 M | **1206** | on HV (below). R32 1 M is C17927, Basic |
| R26 | 10 ohm | 1206 | firing current sense |
| Rcs1 | 2.2 ohm | 0805 | C17521, Basic. About 0.05 W against the 0.125 W rating |
| R11, R45, R46 | 1 ohm, in parallel (0.33 ohm) | 1206 | C17928, Basic. No 0.33 ohm part is Basic. Under 30 mW each against 0.25 W |

| Capacitors that need care | Value | Requirement |
|---|---|---|
| Ct1, C8 | 2.2 nF, 1 nF | **C0G/NP0**: they set U3's and U2's clocks. X7R loses 10-15 % with temperature and more with voltage |
| Cf1, Cp1 | 470 pF, 100 pF | **C0G/NP0**: current-sense filter and compensation pole |
| Cvcc1 | 0.1 uF | **50 V**: on the 20 V rail, where a 16 or 25 V 0805 loses most of its capacitance |
| Cz1 | 100 nF | X7R, 0805 50 V: C49678, Basic, the same part as C17. Sets the compensation zero (1.6 Hz) |

**Cable-entry and filter parts**

| Ref | Function | Part | JLCPCB |
|---|---|---|---|
| R40, R41 | I2C series, 100 ohm 0805 | 0805W8F1000T5E (C17408) | Basic |
| R44 | AIN2 series, 1 k 0805 | 0805W8F1001T5E (C17513) | Basic |
| R42, R43 | I2C pull-ups, 4.7 k 0805, not fitted | 0805W8F4701T5E (C17673) | Basic |
| C18, C19 | AIN0 and AIN1 filters, 100 nF 0805 | CC0805KRX7R9BB104 (C49678) | Basic |
| C21, C22, C23 | HV bypass, 3 in series, 100 nF 50 V X7R, 1206 | CL31B104KBCNNNC (C24497) | Basic |

No Basic part fits C5/C7/C14/C20 or F1, so both are Extended (above). The only Basic 100 uF
parts are 6.3 V: a ceramic that wouldn't damp the cable, and a tantalum too close to 5 V.

**U4 is optional.** It only adds readings (coin-board-schematic.md, section 9). It's one
of the under-$2 [ADS1115 modules](https://www.aliexpress.us/item/2251832488241448.html) on a
header, not the bare chip. These modules usually carry their own I2C pull-ups and decoupling,
which is fine here at VIO.

### UC3843 alternatives

The 8-pin UC3842/3/4/5 family all share one pinout, and the "43" is the one with the
8.4 V start / 7.6 V stop threshold this board needs.

| Part | Maker | Package | $ (qty 1) | Drop-in? |
|---|---|---|---:|---|
| **UC3843BD1R2G** | onsemi | SOIC-8 | 0.71 | the part on the board |
| UC3843BD1 | ST | SOIC-8 | | yes, ST's version of the same part |
| UCC38C43DR, UCC28C43DR, -DGKR | TI | SOIC-8 / VSSOP-8 | 0.90-2.52 | **no**: the BiCMOS UCC28C4x/38C4x are rated **20 V absolute maximum, 18 V recommended** for VDD ([TI datasheet SLUS458](https://www.ti.com/document-viewer/UCC38C43/datasheet/GUID-7D2F25E7-C9BF-401E-BD1E-0AAA9B0B1602)), and U3's VCC is the 20 V rail |
| UC3843AD | TI | **14-pin SOIC** | 2.49 | **no**: TI's "D" UC3843A is the 14-pin version with a different pinout |

Also not drop-ins: the **UC3842** (16 V start / 10 V stop, too close to the 20 V rail)
and the **UC3844/UC3845** (output limited to 50 % duty, too low for this boost).

### What it costs

Estimated 2026-11-04 for a board about the size of the Analog Redial (115 x 54 mm), 2 layers,
top-side assembly only, JLCPCB Economic, delivered to the US. JLCPCB part prices are its own
price breaks at the order's real quantities (232 solder joints a board). DigiKey prices are its
breaks where shown, and the qty-1 price where not. **Estimates, not a quote:** the PCB and
shipping figures for 10 and 30 boards are scaled from the 5-board Analog Redial quote
([docs/board.md](../docs/board.md)), and the fees are JLCPCB's as I know them: $8 setup, $1.50
stencil, $0.0017 a joint, **$3 per unique Extended part per order** (Preferred parts have no fee).

| | 5 boards | 10 boards | 30 boards |
|---|---:|---:|---:|
| PCB | 8.10 | ~11 | ~25 |
| SMT parts (JLCPCB places all of them) | 26.74 | 48.73 | 132.44 |
| Extended fees, 17 parts | 51.00 | 51.00 | 51.00 |
| Setup, stencil, placement | 11.47 | 13.44 | 21.33 |
| **JLCPCB merchandise** | **97.31** | **124.18** | **229.77** |
| Shipping | 30.86 | ~35 | ~50 |
| Sales tax 8.625 %, tariff about 47.5 % of merchandise | 54.61 | 69.69 | 128.96 |
| **JLCPCB delivered** | **182.78** | **228.87** | **408.73** |
| DigiKey hand-solder parts (C1, K1, Qa1, J1-J3 with mating plugs, J6-J9, shunts, U4 socket), with tax and about $7 shipping | 79.85 | 142.87 | 407.27 |
| U4, ADS1115 module (AliExpress, about $2) | 10 | 20 | 60 |
| **Order total** | **~273** | **~392** | **~876** |
| **Per board** | **~$55** | **~$39** | **~$29** |
| Per board with the six swaps below | ~$49 | ~$36 | ~$28 |

The Extended fee is fixed per order, so it's most of the gap between 5 and 30 boards: $10 a
board at 5, $1.70 at 30. Six of the 17 have a no-fee part that drops in:

| Ref | Now (Extended) | Swap | Change |
|---|---|---|---|
| Ct1 | 2.2 nF C0G, not stocked as specified | CL21C222JBFNNNE (C28260), Basic | none |
| Cp1 | 100 pF C0G (C62768) | CL21C101JBANNNC (C1790), Basic | none |
| D5 | BZT52C22 (C173423) | MMSZ5251B (C22395560), Preferred | none, SOD-123 |
| D6 | BZT52C12 (C173429) | MMSZ5242B (C19077426), Preferred | none, SOD-123 |
| Dz1 | SMAZ15 (C110524) | 1SMA4744A (C19077482), Preferred | none, SMA |
| D1 | 1N5819HW (C82544) | SS34 (C8678), Basic, as on the v6 board | footprint to SMA |

The other 11 have no Basic equivalent: C5/C7/C14/C20, L1, L2, F1, U1, U3, Q3, D3, C8 and Cf1
(C0G; Basic has only X7R at 1 nF and 470 pF), and R12 (82 ohm). U2, D2/D14 (ES2D), Q5 and R17
are Preferred, so they cost no fee. Hand-soldering some Extended parts yourself still saves $3
each per order.

The biggest single items are the hand-solder parts: K1 ($3.84-4.52), C1 ($3.08) and Qa1
($2.43). The DigiKey figures for C1, Qa1, the JST parts and the pin strip had no quantity
breaks on the page, so they're qty-1 prices and a little high at 30.

## Will small resistors and capacitors stand the voltage?

**Resistors.** Thick-film chips have a maximum working voltage, separate from their power
rating. Typical figures (UniOhm/Yageo-class general parts; check the datasheet of the exact
part):

| Size | Max working voltage (typ.) | Power (typ.) |
|---|---|---|
| 0603 | 50-75 V | 1/10 W |
| 0805 | 150 V | 1/8 W |
| 1206 | 200 V | 1/4 W |
| 2512 | 200 V | 1 W |

Where it matters on this board, at 119 V:

- **Rfbt1-3, Rbl1-3:** 3 in series, about 40 V each.
- **R27, R28:** 2 in series, about 47 V each.
- **R32:** a single resistor, about 115 V, inside a 1206's 200 V.

All are 1206: besides the rating, the wider pad gap helps with HV clearance. Everything else
(logic, 20 V, gate drive) is fine in 0805.

**Capacitors.** On HV, C1 is rated for it, and C21-C23 share the voltage in series (about
40 V each at 119 V). For the rest, pick a rated voltage at least 2x
the working voltage, and mind DC bias.

## PCB layout and routing

The parts in `Payphone_v1.kicad_pcb` start out roughly in the blocks below, ungrouped,
with room left for routing. J1, J2, J3 and J6 are locked. There is no board outline
yet: draw Edge.Cuts once the mounting is decided.

On the board:

- **F.Silkscreen** (fabricated): each block's name, the HV warning by C1 ("stays charged
  until the HV LED is out"), and the J7-J9 voltage table.
- **User.Comments** (not fabricated): a dashed outline around each block and
  the routing notes below, next to the block they're about. Hide the layer
  when it gets in the way.

| Block | Parts | Sits |
|---|---|---|
| LOGIC | U1, C16, U4, C17, R17, R19, R33 | top left, beside J6/J1 |
| (not placed yet) | JP1; debug LEDs D8-D13 with R34-R39; HV LED R27, R28, D5, D7 | see "Still to do" |
| COIN | R4-R9, C2-C4 | top right, at J3 |
| STAGE1 (5 V -> 20 V) | U2, L1, D1, R11-R14, R45, R46, Q1, R1, C5-C8 | bottom left |
| STAGE2 (20 V -> HV, with C1) | U3 and its small parts, Qa1, L2, C14, D2, Rcs1, Dz1, Rg1, J7-J9, Rfbb1-4, Rfbt1-3, C1, D3, R32, Rbl1-3 | bottom middle |
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

- **Hot loop:** U2 pin 1 (SwC) -> D1 -> C7 -> GND -> U2 pin 2. Smallest
  possible, all on top.
- **Input:** J6 +5V -> C5/C6 -> R11/R45/R46 -> U2 pin 7 and L1. Put the three side by side, with short equal paths, so they share the current. Wide traces (1 mm or
  more): this draws about 1 A peaks from the main board.
- **SW1** (L1, D1 anode, U2 pin 1): short, with little copper.
- C8 at pin 3; R13/R14 at the FB pin (pin 5).
- 20V leaves from C7 to stage 2 (L2, U3 pin 7), and up to Q5/R23 in the
  FIRE block. C14 sits at L2 and Qa1's source return, not back at stage 1.

### Stage 2 (UC3843)

- **Hot loop:** Qa1 drain -> D2 -> C1+ ... C1- -> Rcs1 -> Qa1 source.
  C1 is large, so this loop is too. **C21-C23** (3 x 100 nF 50 V in series) close the
  fast part of it: run them from D2's cathode to Rcs1's GND end. Their middle nodes tie to
  HV_B1 and HV_B2, so Rbl1-3 share the voltage, about 40 V each at 119 V.
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
- **C18 and C19** (100 nF, AIN0 and AIN1 to GND) go at U4: the taps are about 30 k and
  8.5 k source impedance, and the caps make them low-impedance to noise. The readings are
  slow, so the delay costs nothing. AIN2 (ISENSE) has no cap, because it measures a pulse.

### Still to do on the board

- **U4's footprint must be fixed first.** `Custom:ADS1115 Module` numbers its pads 1-10 in
  header order, but the ADS1115 symbol uses the bare chip's pin numbers, so the module's VDD pad
  gets GND and every other pin is wrong too. Renumber the pads, in header order, to
  **8, 3, 10, 9, 1, 2, 4, 5, 6, 7** (VDD, GND, SCL, SDA, ADDR, ALRT, A0, A1, A2, A3), then
  update the footprint on the board.
- **Update PCB from Schematic** for D14, R44 and J1/J6's new pin order, which moves their nets.
- **Place what's new:** F1, C20, R40-R43 by J1 and J6, with R40 and R41 closest to the
  connectors; J5 near U1; D14 by K1; R44 at U4; C21-C23 per the stage 2 notes above; Cp1 at U3
  pins 1 and 2; and:
  - **HV LED** (R27, R28, D5, D7): beside C1, with D7 where it can be seen with the board
    installed. R27/R28 are HV class.
  - **Debug LEDs** D8-D13 with R34-R39: in a row along an edge, labelled on
    silkscreen 5c / 10c / 25c / HV EN / POL / FIRE. D8-D10 and R34-R36 near
    J3, D11-D13 and R37-R39 near U1.
  - **JP1** (VIO = 5V) near J6, with its label on silkscreen.
- **Pad-to-pad clearance inside footprints:** with the HV class's 1.0 mm,
  DRC flags the pin spacing of K1 (0.54 mm between neighbouring pads), J2
  (0.8 mm), Qa1 (0.64 mm) and Q3 (0.8 mm). These are the parts' own pitches.
  Either add a rule that relaxes clearance between pads of the same footprint,
  or exclude them in DRC. K1's pad 3 (HV) to pad 4 (COIL_A) at 0.54 mm is under
  IPC-2221's 0.6 mm for bare external copper at 101-150 V; smaller pads on
  K1's contact pins would bring it over.
- Silkscreen pin 1 and the signal names at J1 and J6. The v6 end is J11, wired pin for pin.
- Board outline and mounting holes (M3, away from HV).
- Test points: 20 V, VIO, HV (a probe pad behind a series resistor), GND.
- Two reference designators overlap (Cz1/Rz1, Ct1/R18): nudge them when
  tidying silkscreen.
