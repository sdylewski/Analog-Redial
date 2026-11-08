# Coin board: parts and PCB layout

**Released for fab, untested.** No board has been built or tested yet.

Companion to [coin-board-schematic.md](coin-board-schematic.md) and
[output-stage-wiring.md](output-stage-wiring.md). Reference designators and
values match `Payphone_coin_board_v1/Payphone_coin_board_v1.kicad_sch`.

The board is surface-mount, with through-hole only for C1, K1, U4's module and the
connectors. JLCPCB places every surface-mount part except seven that are hand-soldered:
**C5, C7, C14, C20, U3, F1 and D3**. Those have no LCSC number in the LCSC field (their
LCSC number is in the Note field), so they're left off the JLCPCB BOM and CPL.

## Parts

**Surface-mount semiconductors**

| Ref | Function | Part | LCSC | JLCPCB |
|---|---|---|---|---|
| U1 | I2C I/O expander | TCA9534APWR, TSSOP-16 | C206010 | Extended |
| U2 | stage 1, 5 V to 20 V | MC34063ADR2G, SOIC-8 | C32078 | Preferred |
| U3 | stage 2 controller | UC3843BD1R2G, SOIC-8 | C16414 | **hand-soldered** |
| Q3, Q6 | fire switch, stage 2 switch, 200 V | STN4NF20L (TECH PUBLIC), SOT-223 | C7603336 | Extended |
| Q1, Q4 | small N-FETs | 2N7002, SOT-23 | C8545 | Basic |
| Q2 | relay driver NPN | MMBT3904, SOT-23 | C20526 | Basic |
| Q5 | gate-drive PNP | MMBT3906, SOT-23 | C7420354 | Preferred |
| D1 | stage 1 Schottky | SS34, 3 A 40 V, SMA | C8678 | Basic |
| D2, D14 | stage 2 ultrafast, coil freewheel | ES2D, SMA | C18199161 | Preferred |
| D3 | HV clamp | SMBJ130A, 600 W unidirectional, SMB | C5353290 | **hand-soldered** |
| D4 | relay flyback | 1N4148W, SOD-123 | C81598 | Basic |
| D5, D6 | zeners, 22 V, 12 V | BZT52C22, BZT52C12, SOD-123 | C22375295, C19077410 | Preferred |
| Dz1 | Q6 gate zener, 15 V 1 W | 1SMA4744A, SMA | C19077482 | Preferred |
| D7 | HV indicator | red LED, 0805 | C84256 | Basic |
| D8-D13, D15 | debug LEDs | green LED, 0805 | C2297 | Basic |
| D16-D19 | coin input blocking diodes | B5819W, 40 V 1 A Schottky, SOD-123 | C8598 | Basic |

**Inductors** (SXN SMDRI127, 12.3 x 12.3 x 8 mm, shielded, footprint
`Inductor_SMD:L_SXN_SMDRI127`; JLCPCB places both)

| Ref | Value | Part | Isat | Heating (40 C rise) | DCR max | Peak here | LCSC |
|---|---|---|---|---|---|---|---|
| L1 | 100 uH | SMDRI127-101MT | 3.5 A | 1.7 A | 0.22 ohm | about 1 A | C10150, Extended |
| L2 | 220 uH | SMDRI127-221MT | 2.0 A | 1.16 A | 0.39 ohm | 0.45 A | C10151, Extended |

**Electrolytics and fuse** (hand-soldered)

| Ref | Function | Part | Notes |
|---|---|---|---|
| C5, C7, C14, C20 | 100 uF / 50 V on +5V and 20V | Nichicon UCD1H101MNL1GS (DigiKey) or Ymin VKMD1001H101MV (LCSC C487360), 8 x 10 mm, footprint `CP_Elec_8x10` | aluminium, not polymer or ceramic: C20 needs some ESR to damp the cable |
| F1 | resettable fuse on +5V | Littelfuse 1812L110PR, 1812 | 1.1 A hold, 2.2 A trip |

**Through-hole** (hand-soldered)

| Ref | Function | Part |
|---|---|---|
| C1 | 220 uF / 200 V | Nichicon UCS2D221MHD, 18 x 26.5 mm, 7.5 mm leads |
| K1 | DPDT relay, 5 V coil | Panasonic TX2-5V (2 A, 220 VDC switching, 140 mW coil). The Omron G6S-2 DC5 fits the same footprint (`Relay_DPDT_Omron_G6S-2`) |
| J2 | relay cable | Molex 0026604030, KK 396, 3-pin |
| J3 | coin switch cable | Molex 0026604040, KK 396, 4-pin |
| J1 or J6 | main board cable | JST B5B-XH-A (mates with XHP-5), or a 1 x 5 pin header, 2.54 mm |
| J7-J9 | HV setting | 1 x 2 pin headers + shunts, 2.54 mm |
| J10 | two-coil relay option (J11 pin 2 to HV) | 1 x 2 pin header + shunt |
| J14 | HV boost: U3's supply | 1 x 2 pin header + shunt. **Shunt for the HD550002C**; leave it off for 12-24 V coils |
| J11 | relay screw terminal (instead of J2) | Phoenix MKDS 1,5/3-5,08 (1715734) |
| J12 | coin input screw terminal (J3 + coin 4) | KF128-2.54-5P, 26-18 AWG (footprint `TerminalBlock_Xinya_XY308-2.54-5P`) |
| J13 | spare analog input | 1 x 3 pin header, 2.54 mm |
| U4 | ADC | ADS1115 module on a 1 x 10 header (VDD, GND, SCL, SDA, ADDR, ALRT, A0-A3), in a socket |

**Not fitted:** R42, R43 (I2C pull-ups, for testing the board alone), Rslope1 and Cslope1
(slope compensation, only if stage 2 shows subharmonic oscillation). JP1 is a solder jumper.

**U4 is optional.** It only adds readings ([coin-board-schematic.md](coin-board-schematic.md),
section 9). It's one of the common ADS1115 modules, not the bare chip; these carry their own
I2C pull-ups and decoupling, which is fine at VIO.

### Resistors and small capacitors

0805, placed by JLCPCB, except:

| Ref | Value | Size | Why |
|---|---|---|---|
| Rfbt1-3, R57-R59, R32 | 100 k, 1 M, 1 M | **1206** | on HV: about 40 V each in the strings, about 115 V across R32 |
| R27, R28 | 33 k | 0805 | HV LED, two in series: about 47 V and 67 mW each, inside an 0805's 150 V and 0.125 W |
| R26 | 10 ohm | 1206 | firing current sense |
| R12 | 100 ohm | **1206** | U2's driver resistor, 0.13 W |
| R11, R45, R46 | 1 ohm, in parallel (0.33 ohm) | 1206 | U2's current sense |
| C21-C23 | 100 nF 50 V X7R, in series | 1206 | HV bypass |

| Capacitor | Value | Requirement |
|---|---|---|
| Ct1 | 2.2 nF | **C0G**: it sets U3's 65 kHz clock |
| Cp1 | 100 pF | **C0G**: compensation pole |
| C8 | 1 nF | X7R: U2's clock, which U2's own current limit makes tolerant of drift |
| Cf1 | 470 pF | X7R: current-sense filter |
| C26 | 100 nF | **50 V or more**: on the 20 V rail (C28233, 100 V, the board's 100 nF part) |
| C28 | 100 nF | X7R: compensation zero (1.6 Hz) |

The full list, with LCSC numbers, is
[`jlcpcb/production_files/BOM-Payphone_coin_board_v1.csv`](Payphone_coin_board_v1/jlcpcb/production_files/BOM-Payphone_coin_board_v1.csv).

### Hand-solder parts from DigiKey

One board's worth. Check stock and price on the page before ordering.

| Ref | Qty | Part | DigiKey |
|---|---:|---|---|
| U3 | 1 | onsemi UC3843BD1R2G, SOIC-8 | [UC3843BD1R2G](https://www.digikey.com/en/products/detail/onsemi/UC3843BD1R2G/921714) |
| D3 | 1 | Littelfuse SMBJ130A, SMB | [SMBJ130A](https://www.digikey.com/en/products/detail/littelfuse-inc/SMBJ130A/688304) |
| F1 | 1 | Littelfuse 1812L110PR, 1812 | [1812L110PR](https://www.digikey.com/en/products/detail/littelfuse-inc/1812L110PR/267851) |
| C5, C7, C14, C20 | 4 | Nichicon UCD1H101MNL1GS, 100 uF 50 V, 8 x 10.5 mm | [UCD1H101MNL1GS](https://www.digikey.com/en/products/detail/nichicon/UCD1H101MNL1GS/2549678) |
| C1 | 1 | Nichicon UCS2D221MHD, 220 uF 200 V | [UCS2D221MHD](https://www.digikey.com/en/products/detail/nichicon/UCS2D221MHD/4991877) |
| K1 | 1 | Panasonic TX2-5V | [TX2-5V](https://www.digikey.com/en/products/detail/panasonic-industry/TX2-5V/251803) |
| J2 | 1 | Molex 0026604030, KK 396 header, 3-pin | [0026604030](https://www.digikey.com/en/products/detail/molex/0026604030/96502) |
| J3 | 1 | Molex 0026604040, KK 396 header, 4-pin | [0026604040](https://www.digikey.com/en/products/detail/molex/0026604040/197447) |
| J1 | 1 | JST B5B-XH-A (or fit J6 instead) | [B5B-XH-A](https://www.digikey.com/en/products/detail/jst-sales-america-inc/B5B-XH-A-LF-SN/1530483) |
| U4 socket | 1 | Sullins PPTC101LFBN-RC, 1 x 10 female | [PPTC101LFBN-RC](https://www.digikey.com/en/products/detail/sullins-connector-solutions/PPTC101LFBN-RC/810149) |
| J12 (optional) | 1 | KF128-2.54-5P screw terminal (not at DigiKey) | [AliExpress](https://www.aliexpress.us/item/3256801737987839.html) |
| U4 | 1 | ADS1115 module (not at DigiKey) | [AliExpress](https://www.aliexpress.us/item/3256806547330256.html) |

Also needed: pin headers and shunts for J6-J10, J13 and J14, and the optional screw terminal
J11 (Phoenix 1715734). J2 and J3 take the phone's own KK 396 plugs.

**U3 substitutes:** ST's UC3843BD1 is the same part. Not substitutes: TI's UCC28C43/UCC38C43
(20 V absolute maximum VDD, and U3 runs from the 20 V rail), TI's UC3843AD (14-pin), the
UC3842 (16 V start) and the UC3844/5 (50 % duty limit).

### Ordering

Upload the files in `Payphone_coin_board_v1/jlcpcb/production_files/` (gerbers, BOM, CPL):

- 2 layers, **lead-free HASL**, top-side assembly, **Economic PCBA** (JLCPCB offers Economic
  only with HASL).
- Four parts carry JLCPCB's Extended-part fee (about $3 each per order): U1, Q3/Q6, L1 and L2.
- Estimated cost for 5 boards, delivered to the US with tariff and tax: about $130 from
  JLCPCB, about $70 for the DigiKey parts and $10 for five ADS1115 modules, so **about $40 a
  board**. An estimate, not a quote.

### The JLCPCB plugin

`Payphone_coin_board_v1/jlcpcb/` is kicad-jlcpcb-tools' output (its `project.db` isn't
tracked). Before each export:

- Every surface-mount part JLCPCB places is in the CPL file, not just the BOM. A symbol marked
  "exclude from position files" carries that to its footprint on Update PCB.
- No hand-solder part has an LCSC number, and the plugin's **"Add parts without LCSC numbers to
  BOM/CPL" is off**, so hand-solder parts are left out of both files.

## Voltage ratings of small parts

Thick-film chip resistors have a maximum working voltage separate from their power rating
(typical general-purpose parts; check the exact part's datasheet):

| Size | Max working voltage (typ.) | Power (typ.) |
|---|---|---|
| 0603 | 50-75 V | 1/10 W |
| 0805 | 150 V | 1/8 W |
| 1206 | 200 V | 1/4 W |

On HV (up to 119 V): Rfbt1-3 and R57-R59 are three in series, about 40 V each; R27 and R28
two in series, about 47 V each; R32 alone, about 115 V. C1 is rated 200 V, and C21-C23 share
the voltage in series (about 40 V each). Everything else (logic, 20 V, gate drive) is well
inside an 0805's rating.

## PCB layout

Two layers, 1 oz copper. Power flows left to right: J6 -> stage 1 -> stage 2 -> C1 -> K1 -> J2.

| Block | Parts | Where |
|---|---|---|
| LOGIC | U1, U4, JP1, F1, C20, R40-R43, debug LEDs D11-D13 | top left, beside J6/J1 |
| COIN | R4-R9, C2-C4, D16-D19, debug LEDs D8-D10, coin 4's R49-R51, C24, D15 | top right, at J3 and J12 |
| STAGE1 (5 V -> 20 V) | U2, L1, D1, R11-R14, R45, R46, Q1, R3, C5-C8 | bottom left |
| STAGE2 (20 V -> HV) | U3 and its small parts, Q6, L2, C14, D2, Rcs1, Dz1, J7-J9, J14, the feedback string, C1, D3, R32, R57-R59, the HV LED | bottom middle |
| FIRE | R1, R21-R25, Q4, Q5, D6, Q3, R26 | right, above K1 |
| POL | K1, Q2, D4, D14, R2, R20, J10, J11 | right, beside J2 |

On the silkscreen: each block's name, the HV warning by C1 ("stays charged until the HV LED
is out"), the J7-J9 voltage table and the debug LED labels. User.Comments (not fabricated)
carries routing notes beside each block.

### Net classes

Set in `Payphone_coin_board_v1.kicad_pro`:

| Net class | Nets | Track | Clearance | Via | Carries |
|---|---|---|---|---|---|
| HighCurrent | +5V_IN, U2 Ipk node, SW1 (D1 anode) | 1.0 mm | 0.25 mm | 0.8 / 0.4 mm | stage 1's 1 A peaks |
| Power | +5V, VIO, 20V, GND | 0.8 mm | 0.2 mm | 0.8 / 0.4 mm | up to about 0.5 A |
| HV | HV, HV_*, SW2, COIL_A/B/C, RETURN | 0.5 mm | 1.0 mm | 0.8 / 0.4 mm | the coin relay pulse, 0.45 A peaks on SW2 |
| Default | everything else | 0.25 mm | 0.2 mm | 0.6 / 0.3 mm | a few mA |

The HighCurrent patterns use KiCad's auto net names (`Net-(U2-Ipk)`, `Net-(D1-A)`): update
them if a re-annotation renames those nets. A logic net whose name starts with HV would pick
up the HV class, so don't name one that way.

**Pads inside one HV part** are checked against 0.6 mm (IPC-2221 B2, external uncoated copper,
101-150 V) by the rules in `Payphone_coin_board_v1.kicad_dru`, for K1, J2, J10, Q3, Q6, R27
and R28, whose pin spacing is fixed by the part. **K1's switch pads (3, 4, 5, 8, 9, 10) are
1.7 mm**, not the library's 2.0 mm, for 0.84 mm between them; DRC lists K1 as differing from
the library for that reason.

### Ground

One ground, no splits: there's no transformer, and FIRE, POL, I2C and the ADC lines all cross
between blocks. The bottom layer is one solid GND pour with only short jumpers. Each power loop
closes on the top layer and stitches to the pour with several vias at its capacitors.

### Stage 1 and stage 2

- **Stage 1 hot loop:** U2 pin 1 -> D1 -> C7 -> GND -> U2 pin 2, all on top. R11/R45/R46 sit
  side by side with equal paths, so they share the current.
- **Stage 2 hot loop:** Q6 drain -> D2 -> C1 ... Rcs1 -> Q6 source. C21-C23 close its fast
  part, from D2's cathode to Rcs1's GND end.
- **SW2** (Q6 drain, L2, D2 anode) swings 0-120 V: short, with nothing running parallel to it.
- **Current sense:** a Kelvin trace from Rcs1's top pad to R56; Cf1 at U3's CS pin.
- **Quiet ground:** Ct1, C27, Cf1, Rfbb4 and J7-J9 return to U3 pin 5, then one via to the
  pour. The FB node is short, at U3 pin 2; HV runs out to Rfbt1, not the other way.
- **R32 sits at C1:** only its 3 V tap (AIN1) runs to U4, so no HV enters the logic block.

### FIRE, POL and the ADC

- FIRE and POL leave U1 as VIO logic lines; the level shift is at Q3, so GATE3 is only a few
  millimetres long.
- **Firing loop:** C1+ -> K1 -> J2 -> coin relay -> K1 -> Q3 -> R26 -> GND -> C1-, compact,
  with R26's GND via toward C1-. K1's HV (3, 8) and RETURN (5, 10) pads cross over, so one pair
  runs on the bottom layer.
- Each ADC divider's low end and filter cap (C18, C19) sits at U4, so the long runs are the 3 V
  taps.
