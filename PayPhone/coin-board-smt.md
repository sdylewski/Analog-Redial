# Coin board: parts and PCB layout

**Released for fab, untested.** The PCB layout is finished (2026-11-08), but no board has been
built or tested yet. The cost estimate below is for planning.

Companion to [coin-board-schematic.md](coin-board-schematic.md) and
[output-stage-wiring.md](output-stage-wiring.md). Reference designators and
values match `Payphone_coin_board_v1/Payphone_coin_board_v1.kicad_sch`.

The board is surface-mount, with through-hole only where it's the better part:
C1, K1, U4's module and the connectors (headers and screw terminals included). Seven surface-mount
parts are hand-soldered too, to save JLCPCB's Extended-part fees: **C5, C7, C14, C20, U3, F1 and D3** (their LCSC number is kept in the schematic's Note field). None of the hand-solder parts
has an LCSC number in the LCSC field, so none lands on the JLCPCB BOM or CPL (see "The JLCPCB plugin").

## Parts

DigiKey qty-1 prices looked up 2026-10-30, unless marked; quantity breaks drop most
of them, and LCSC is usually at or below DigiKey. "Basic" is confirmed only where
marked; **(verify)** means my expectation, from memory or common use, so check it in
the BOM upload.

**Surface-mount semiconductors** (JLCPCB places all of them; prices and stock checked 2026-11-04)

| Ref | Function | Part | $ (JLCPCB) | LCSC | Fee |
|---|---|---|---:|---|---|
| U1 | I2C I/O expander | TCA9534APWR, TSSOP-16 | 1.26 | C206010 | Extended |
| U2 | stage 1, 5 V to 20 V | MC34063ADR2G, SOIC-8 | 0.47 | C32078 | Preferred, no fee |
| U3 | stage 2 controller | UC3843BD1R2G, SOIC-8 | 0.37 | C16414 | Extended: **hand-soldered** |
| Q3, Qa1 | fire switch, stage 2 switch, 200 V | STN4NF20L (TECH PUBLIC), SOT-223 | 0.33 | C7603336 | Extended (one fee for both). ST's own is C183261, $0.61 |
| Q1, Q4 | small N-FETs | 2N7002, SOT-23 | 0.02 | C8545 | Basic |
| Q2 | relay driver NPN | MMBT3904, SOT-23 | 0.01 | C20526 | Basic |
| Q5 | gate-drive PNP | MMBT3906, SOT-23 | 0.01 | C7420354 | Preferred, no fee |
| D1 | stage 1 Schottky | SS34, 3 A 40 V, SMA | 0.04 | C8678 | Basic (as on the v6 board) |
| D2, D14 | stage 2 ultrafast, coil freewheel | ES2D, SMA | 0.03 | C18199161 | Preferred, no fee |
| D3 | HV clamp | SMBJ130A (FOSAN), 600 W unidirectional, SMB | 0.04 | C5353290 | Extended: **hand-soldered** (Littelfuse SMBJ130A from DigiKey) |
| D4 | relay flyback | 1N4148W, SOD-123 | 0.01 | C81598 | Basic |
| D5, D6 | zeners, 22 V, 12 V | BZT52C22, BZT52C12 (hongjiacheng), SOD-123 | 0.02 | C22375295, C19077410 | Preferred, no fee |
| Dz1 | Qa1 gate zener, 15 V 1 W | 1SMA4744A, SMA | 0.06 | C19077482 | Preferred, no fee |
| D7 | HV indicator | red LED, 0805 | 0.01 | C84256 | Basic |
| D8-D13, D15 | debug LEDs | green LED, 0805, KT-0805G | 0.02 | C2297 | Basic |
| D16-D19 | coin input blocking diodes | B5819W SL, 40 V 1 A Schottky, SOD-123 | 0.01 | C8598 | Basic |

The Basic LEDs were checked on JLCPCB's part pages on 2026-10-31. No 1206 red LED is
Basic, so 0805 is the largest for D7; the 0603 greens (C72043, C12624) are Extended.

**Surface-mount inductors**

Both are SXN's SMDRI127 (12.3 x 12.3 x 8 mm, shielded), on KiCad's
`Inductor_SMD:L_SXN_SMDRI127` footprint. JLCPCB prices and stock checked 2026-11-03.
SXN rates saturation at a 25 % drop in inductance.

| Ref | Value | Part | Isat | Heating (40 C rise) | DCR max | Peak here | $ (JLCPCB, qty 1) | LCSC / JLCPCB | Fit |
|---|---|---|---|---|---|---|---:|---|---|
| L1 | 100 uH | SMDRI127-101MT | 3.5 A | 1.7 A | 0.22 ohm | about 1 A (R11/R45/R46) | 0.23 | C10150, Extended, 64,000 in stock | **JLCPCB** (DigiKey doesn't stock it) |
| L2 | 220 uH | SMDRI127-221MT | 2.0 A | 1.16 A | 0.39 ohm | 0.45 A (Rcs1) | 0.23 | C10151, Extended, 17,000 in stock | **JLCPCB** (DigiKey doesn't stock it) |

Each is an Extended part, so JLCPCB adds its per-part fee (about $3 per order) on top.

**Surface-mount electrolytic and fuse** (Extended at JLCPCB, so **hand-soldered**; checked 2026-11-04)

| Ref | Function | Part | $ | LCSC | Why this one |
|---|---|---|---:|---|---|
| C5, C7, C14, C20 | 100 uF / 50 V on +5V and 20V | Ymin VKMD1001H101MV (from DigiKey: Nichicon UCD1H101MNL1GS, see "DigiKey order"), 8 x 10 mm, footprint `CP_Elec_8x10` | 0.24 | C487360 | 724 mA ripple at 100 kHz against about 0.25 A in C7 and 0.15 A in C14; 10,000 h at 105 C. Aluminium, not polymer: C20 needs some ESR to damp the cable. One part for all four |
| F1 | resettable fuse on +5V | Littelfuse 1812L110PR, 1812 | 0.05 | C163516 | 1.1 A hold, 2.2 A trip, 8 V (the same hold and trip as the MF-R110 it replaces) |

Ceramics don't suit the 100 uF positions: a 22 uF 35 V X5R keeps only about a third of its
capacitance at 20 V, and has too little ESR for C20.

**Through-hole, hand-soldered ("do not place" for JLCPCB)**

| Ref | Function | Part | $ |
|---|---|---|---:|
| C1 | 220 uF / 200 V | Nichicon UCS2D221MHD, 18 x 26.5 mm, 7.5 mm leads | 3.08 |
| K1 | DPDT relay, 5 V coil | Panasonic TX2-5V: 2 A, 220 VDC max switching, 140 mW coil. Same pins and coil polarity as the Omron G6S-2 DC5 (footprint `Relay_DPDT_Omron_G6S-2`), which also fits | 1.95 |
| J2 | relay cable | Molex 0026604030, KK 396, 3-pin (plug: housing 0009503031 + 3 x 0008500106 terminals) | 0.35 |
| J3 | coin switch cable | Molex 0026604040, KK 396, 4-pin (plug: housing 0009503041 + 4 x 0008500106 terminals) | 0.44 |
| J1 or J6 | main board cable | JST B5B-XH-A (mates with XHP-5), or a 1 x 5 pin header, 2.54 mm | |
| J7-J9 | HV setting | 1 x 2 pin headers + shunts, 2.54 mm | |
| J10 | two-coil relay option (J11 pin 2 to HV) | 1 x 2 pin header + shunt, 2.54 mm | |
| J14 | HV boost: U3's supply | 1 x 2 pin header + shunt, 2.54 mm. **Shunt for the HD550002C**; leave it off for 12-24 V coils | |
| J11 | relay screw terminal (instead of J2) | Phoenix MKDS 1,5/3-5,08 (1715734) **(verify price)** | |
| J12 | coin input screw terminal (J3 + coin 4) | KF128-2.54-5P, 2.54 mm pitch, 26-18 AWG (footprint `TerminalBlock_Xinya_XY308-2.54-5P`) | |
| J13 | spare analog input | 1 x 3 pin header, 2.54 mm | |
| U4 | ADC | ADS1115 module on a 1 x 10 header, 2.54 mm (VDD, GND, SCL, SDA, ADDR, ALRT, A0-A3) | under 2 |
| R42, R43, Rslope1, Cslope1 | I2C pull-ups, slope compensation | **not fitted** | |

These come to about **$16.50** a board at DigiKey qty-1 prices, U4's module included (section "What it costs").

### DigiKey order (hand-solder parts)

Everything JLCPCB doesn't place, one board's worth. Links checked 2026-11-08; check stock and
price on the page before ordering. The C5/C7/C14/C20 part in the table above (Ymin, LCSC
C487360) isn't sold at DigiKey: the Nichicon below fits the same `CP_Elec_8x10` footprint, with
350 mA ripple at 100 kHz against about 0.25 A in C7.

| Ref | Qty | Part | DigiKey |
|---|---:|---|---|
| U3 | 1 | onsemi UC3843BD1R2G, SOIC-8 | [UC3843BD1R2G](https://www.digikey.com/en/products/detail/onsemi/UC3843BD1R2G/921714) |
| D3 | 1 | Littelfuse SMBJ130A, SMB | [SMBJ130A](https://www.digikey.com/en/products/detail/littelfuse-inc/SMBJ130A/688304) |
| F1 | 1 | Littelfuse 1812L110PR, 1812 | [1812L110PR](https://www.digikey.com/en/products/detail/littelfuse-inc/1812L110PR/267851) |
| C5, C7, C14, C20 | 4 | Nichicon UCD1H101MNL1GS, 100 uF 50 V, 8 x 10.5 mm, 5000 h at 105 C | [UCD1H101MNL1GS](https://www.digikey.com/en/products/detail/nichicon/UCD1H101MNL1GS/2549678) |
| C1 | 1 | Nichicon UCS2D221MHD, 220 uF 200 V, 18 mm, 7.5 mm leads | [UCS2D221MHD](https://www.digikey.com/en/products/detail/nichicon/UCS2D221MHD/4991877) |
| K1 | 1 | Panasonic TX2-5V, 5 V coil | [TX2-5V](https://www.digikey.com/en/products/detail/panasonic-industry/TX2-5V/251803) |
| J2 | 1 | Molex 0026604030, KK 396 header, 3-pin | [0026604030](https://www.digikey.com/en/products/detail/molex/0026604030/96502) |
| J3 | 1 | Molex 0026604040, KK 396 header, 4-pin | [0026604040](https://www.digikey.com/en/products/detail/molex/0026604040/197447) |
| J1 | 1 | JST B5B-XH-A, 5-pin XH (or fit J6 instead) | [B5B-XH-A](https://www.digikey.com/en/products/detail/jst-sales-america-inc/B5B-XH-A-LF-SN/1530483) |
| U4 socket | 1 | Sullins PPTC101LFBN-RC, 1 x 10 female, 2.54 mm | [PPTC101LFBN-RC](https://www.digikey.com/en/products/detail/sullins-connector-solutions/PPTC101LFBN-RC/810149) |
| J12 (optional) | 1 | KF128-2.54-5P screw terminal, 5 pins, 2.54 mm (not from DigiKey) | [AliExpress](https://www.aliexpress.us/item/3256801737987839.html) |
| U4 | 1 | ADS1115 module, pins VDD, GND, SCL, SDA, ADDR, ALRT, A0-A3 (not from DigiKey) | [AliExpress](https://www.aliexpress.us/item/3256806547330256.html) |

Not on the list: plugs for J2 and J3 (reuse the phone's own KK 396 plugs on the relay and coin
switch cables), the pin headers and shunts for J6-J10, J13 and J14, and the optional screw
terminal J11 (Phoenix 1715734). JP1 is a solder jumper (no part), and
R42, R43, Rslope1 and Cslope1 aren't fitted.

**Resistors and small capacitors: 0805, JLCPCB places them**, except:

| Ref | Value | Size | Why |
|---|---|---|---|
| Rfbt1-3, Rbl1-3, R32 | 100 k, 1 M, 1 M | **1206** | on HV (below). R32 1 M is C17927, Basic |
| R27, R28 | 33 k | 0805 | HV LED, two in series: about 47 V and 67 mW each, inside an 0805's 150 V and 0.125 W. C17633, Basic |
| R26 | 10 ohm | 1206 | firing current sense |
| Rcs1 | 2.2 ohm | 0805 | C17521, Basic. About 0.05 W against the 0.125 W rating |
| R12 | 100 ohm | **1206** | C17901, Basic. U2's driver resistor: about 4 V across it for 80 % of each cycle, 0.13 W, too much for an 0805 |
| R11, R45, R46 | 1 ohm, in parallel (0.33 ohm) | 1206 | C17928, Basic. No 0.33 ohm part is Basic. Under 30 mW each against 0.25 W |

| Capacitors that need care | Value | Requirement |
|---|---|---|
| Ct1 | 2.2 nF | **C0G** (C28260, Basic): it sets U3's 65 kHz clock, which the DCM sizing depends on |
| C8 | 1 nF | X7R (C46653, Basic). It sets U2's clock; X7R's 10-15 % drift moves that a little, which U2's own current limit makes harmless. C0G at 1 nF is Extended only |
| Cp1 | 100 pF | **C0G** (C1790, Basic): compensation pole |
| Cf1 | 470 pF | X7R (C1743, Basic): current-sense filter, where 10-15 % doesn't matter. C0G at 470 pF is Extended only |
| Cvcc1 | 0.1 uF | **50 V or more**: on the 20 V rail, where a 16 or 25 V 0805 loses most of its capacitance. C28233 (100 V, Basic), the board's other 100 nF |
| Cz1 | 100 nF | X7R, 0805: C28233 (100 V, Basic), the board's other 100 nF. Sets the compensation zero (1.6 Hz) |

**Cable-entry and filter parts**

| Ref | Function | Part | JLCPCB |
|---|---|---|---|
| R40, R41 | I2C series, 100 ohm 0805 | 0805W8F1000T5E (C17408) | Basic |
| R44 | AIN2 series, 1 k 0805 | 0805W8F1001T5E (C17513) | Basic |
| R52, C25 | AIN3 (J13) filter, 10 k and 100 nF 0805 | C17414, C28233 | Basic |
| R53 | Qa1 gate to source, 10 k | C17414 | Basic |
| R49-R51, C24 | coin 4: 10 k pull-up, 1 k series, 1.5 k LED, 100 nF | C17414, C17513, C4310, C28233 | Basic |
| R42, R43 | I2C pull-ups, 4.7 k 0805, not fitted | 0805W8F4701T5E (C17673) | Basic |
| C18, C19 | AIN1 (HV) and AIN0 (20 V) filters, 100 nF 0805 | CL21B104KCFNNNE (C28233) | Basic |
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
top-side assembly only, JLCPCB Economic, delivered to the US, with the parts as chosen above.
JLCPCB part prices are its own price breaks at the order's real quantities (232 solder joints a
board). DigiKey prices are its breaks where shown, and the qty-1 price where not. **Estimates,
not a quote:** the PCB and shipping figures for 10 and 30 boards are scaled from the 5-board
Analog Redial quote ([docs/board.md](../docs/board.md)), and the fees are JLCPCB's as I know
them: $8 setup, $1.50 stencil, $0.0017 a joint, **$3 per unique Extended part per order**
(Basic and Preferred parts have no fee).

| | 5 boards | 10 boards | 30 boards |
|---|---:|---:|---:|
| PCB | 8.10 | ~11 | ~25 |
| SMT parts (JLCPCB places all of them) | 25.94 | 48.46 | 134.41 |
| Extended fees, 8 parts | 24.00 | 24.00 | 24.00 |
| Setup, stencil, placement | 11.50 | 13.51 | 21.53 |
| **JLCPCB merchandise** | **69.54** | **96.98** | **204.94** |
| Shipping | 30.86 | ~35 | ~50 |
| Sales tax 8.625 %, tariff about 47.5 % of merchandise | 39.03 | 54.43 | 115.02 |
| **JLCPCB delivered** | **139.43** | **186.41** | **369.96** |
| DigiKey hand-solder parts (C1, K1, J1-J3 with mating plugs, J6-J9, shunts, U4 socket), with tax and about $7 shipping | 66.65 | 116.47 | 328.08 |
| U4, ADS1115 module (AliExpress, about $2) | 10 | 20 | 60 |
| **Order total** | **~216** | **~323** | **~758** |
| **Per board** | **~$43** | **~$32** | **~$25** |

**Not in the table yet:** the universal-payphone parts (D15-D19, R49-R53, C24, C25)
are all Basic, so no new fee, and add roughly $0.15 a board at JLCPCB. The screw terminals
J11 and J12 and the J10/J13/J14 headers add a few dollars a board at DigiKey **(verify)**, and
they're optional: fit them only for phones that need them.

The 8 parts with an Extended fee have no Basic or Preferred equivalent: C5/C7/C14/C20, L1, L2,
F1, U1, U3, Q3/Qa1 and D3. Hand-soldering one yourself saves its $3 per order. **Now hand-soldered:**
C5/C7/C14/C20, F1, U3 and D3. L1 and L2 stay with JLCPCB, since DigiKey doesn't stock them, which
leaves four fees: U1, Q3/Qa1, L1 and L2 ($12). R17 was 56 k, an
Extended part; it's now 150 k with R19 27 k (the AIN0 divider, 6.56:1), both values already Basic
parts on the board. The table above still has the earlier 8-fee figures.

**The first real quote (2026-11-07, 5 boards, Standard PCBA): $155.94 before shipping and
tariff.** PCB $10, setup $25.75, stencil $8.27, parts $31.72 (49 lines), **feeder loading
$75.95** (49 x $1.55: Standard PCBA charges every unique part, Basic ones too), placement
$3.74, packaging $0.51. The same board as **Economic PCBA** would be about $10 + $8 setup +
$1.50 stencil + $31.72 parts + $27 (9 parts with the $3 Extended fee: the four 100 uF, D3, F1,
L1, L2, Q3/Qa1, R17, U1, U3; Preferred parts have none) + $4 = **about $82**. Use Economic
unless JLCPCB won't offer it for the board; on Standard, hand-soldering a part saves only its
$1.55 line fee and its price.

**Hand-soldering an Extended part instead** (5 boards, Economic PCBA). What JLCPCB charges for it, delivered,
is its $3 fee plus the parts, times about 1.56 for tariff and tax. Bought from DigiKey with the
through-hole parts, it adds the parts and 8.6 % tax, and no extra shipping. DigiKey prices
below are **estimates (verify)**, at the quantity one order needs.

| Ref | Part | JLCPCB, delivered | DigiKey instead | Saves an order | Hand soldering |
|---|---|---:|---|---:|---|
| U1 | TCA9534APWR, TSSOP-16 | 14.50 | TI TCA9534APWR, about $1.40: 7.60 | **~7** | hardest on the board: 0.65 mm pitch, needs flux and a fine tip |
| C5, C7, C14, C20 | 100 uF 50 V, 8 x 10 mm | 12.20 | Panasonic EEE-FK1H101P (8 x 10.2, same footprint), about $0.40 at 20: 8.70 | **~4** | easy: two big pads |
| U3 | UC3843BD1R2G, SOIC-8 | 7.60 | the same part, $0.71: 3.90 | **~4** | easy |
| F1 | 1812L110PR, 1812 | 5.10 | the same part, about $0.40: 2.20 | **~3** | easy |
| D3 | SMBJ130A, SMB | 5.00 | Littelfuse SMBJ130A, about $0.40: 2.20 | **~3** | easy |
| L1, L2 | SMDRI127, 12 x 12 mm | 6.50 each | Bourns SRR1260-101M / -221M, about $1.00: 5.40 each | ~1 each | easy, but check the Bourns footprint against `L_SXN_SMDRI127` |
| Q3, Qa1 | STN4NF20L, SOT-223 | 9.80 | ST STN4NF20L, about $0.90 at 10: 9.80 | ~0 | easy |

All eight come to about **$22 an order of 5, about $4.50 a board**. U1, the capacitors, U3, F1
and D3 are most of it (about $21); L1, L2 and Q3/Qa1 aren't worth it. U2 is Preferred, so it has
no fee and nothing to save. To move a part to hand soldering, clear its LCSC field (and mark it
"exclude from position files"), then add it to the DigiKey order.

The biggest single items are the hand-solder parts: C1 ($3.08) and K1 ($1.95 for the TX2-5V; the
Omron G6S-2 it replaced was $3.84-4.52). The
DigiKey figures for C1, the JST parts and the pin strip had no quantity
breaks on the page, so they're qty-1 prices and a little high at 30.

### The JLCPCB plugin

`Payphone_coin_board_v1/jlcpcb/` (kicad-jlcpcb-tools: its database and exports, not tracked in git)
should match the schematic's LCSC fields. Check before each upload:

- Every surface-mount part is in the CPL file, not just the BOM. A symbol marked "exclude from
  position files" carries that to its footprint on Update PCB, and JLCPCB then buys the part but
  doesn't place it.
- No hand-solder part has an LCSC number, and in the plugin's Settings **"Add parts without LCSC
  numbers to BOM/CPL" is off**: then hand-solder parts are left out of both files. With it on, they
  land on the BOM without a CPL entry, and JLCPCB warns that they "don't exist in the CPL file".
- When ordering: **lead-free HASL (or HASL)**. JLCPCB offers Economic PCBA only with HASL; with OSP
  or ENIG it's Standard PCBA, about twice the price.

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

The parts in `Payphone_coin_board_v1.kicad_pcb` start out roughly in the blocks below, ungrouped,
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
| COIN | R4-R9, C2-C4, D16-D19, and coin 4's R49-R51, C24, D15 | top right, at J3 and J12 |
| STAGE1 (5 V -> 20 V) | U2, L1, D1, R11-R14, R45, R46, Q1, R3, C5-C8 | bottom left |
| STAGE2 (20 V -> HV, with C1) | U3 and its small parts, Qa1, L2, C14, D2, Rcs1, Dz1, Rg1, J7-J9, Rfbb1-4, Rfbt1-3, C1, D3, R32, Rbl1-3 | bottom middle |
| FIRE | R1, R21-R25, Q4, Q5, D6, Q3, R26 | right, above K1 |
| POL | K1, Q2, D4, R2, R20, J10, J11 | right, beside J2 |

Power flows left to right: J6 -> stage 1 -> stage 2 -> C1 -> K1 -> J2.

### Track widths and vias

The net classes in `Payphone_coin_board_v1.kicad_pro` set these, so the router picks the width
from the net (1 oz copper; the currents are peaks):

| Net class | Nets | Track | Clearance | Via | Carries |
|---|---|---|---|---|---|
| HighCurrent | +5V_IN, U2 Ipk node, SW1 (D1 anode) | 1.0 mm | 0.25 mm | 0.8 / 0.4 mm | stage 1's 1 A peaks |
| Power | +5V, VIO, 20V, GND | 0.8 mm | 0.2 mm | 0.8 / 0.4 mm | up to about 0.5 A |
| HV | HV, HV_*, SW2, COIL_A/B/C, RETURN | 0.5 mm | 1.0 mm | 0.8 / 0.4 mm | the coin relay pulse, 0.45 A peaks on SW2 |
| Default | everything else: I2C, FIRE/POL, gates, ADC, LEDs | 0.25 mm | 0.2 mm | 0.6 / 0.3 mm | a few mA |

- 0.2 mm (in the width list) is fine for any signal line where space is tight; JLCPCB's
  standard process goes down to 0.13 mm at no extra cost.
- Power and HV tracks can narrow to 0.5 mm to get past a pin, and short 20V branches
  (Q5/R23, U3 VCC) can be 0.4 mm. The 20V rail is about 100 mA, so 0.8 mm is
  for low resistance, not heat.
- One 0.4 mm via carries about 1 A; use two or more where a HighCurrent or
  hot-loop path changes layers, and several at each power capacitor's GND.
- The HighCurrent patterns use KiCad's auto net names (`Net-(U2-Ipk)`,
  `Net-(D1-A)`). If a re-annotation renames them, update the patterns.

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
  frequency: short, and nothing parallel to it. Qa1's SOT-223 tab is SW2 too, so give
  it only the copper the footprint has: it dissipates about 0.05 W and needs no heatsink area.
- **Gate:** U3 OUT -> Rg1 -> Qa1 gate, returning through Qa1 source / Rcs1 to
  U3's GND. Dz1 right at the gate and source.
- **Current sense:** a separate (Kelvin) trace from Rcs1's top pad to Rf1;
  Cf1 at U3's CS pin.
- **Quiet ground:** Ct1, Cvref1, Cf1, Rfbb4 and J7-J9 go to U3 pin 5 (GND),
  then one via to the plane. RT/CT noise is what upsets UC384x designs.
- **Feedback:** the FB node is short, with Rfbb1-4, Rz1 and Rfbt3 at U3 pin 2.
  HV runs out to Rfbt1; the high-impedance FB node doesn't run out to HV.

### HV

- HV, HV_*, SW2, COIL_A/B/C and RETURN are in the **HV net class** (1.0 mm
  clearance), so DRC enforces it. J10 and J11 carry HV: keep them by J2 and K1.
- **Pads inside one part** are checked against 0.6 mm instead (IPC-2221 B2,
  external uncoated copper, 101-150 V), by the rules in `Payphone_coin_board_v1.kicad_dru`
  for K1, J2, J10, Q3, Qa1, R27 and R28: their pin spacing is fixed by the part
  (SOT-223 and 1206 pads are 0.8 mm apart). A new HV part with the same issue
  gets its own rule there.
- **K1's switch pads (3, 4, 5, 8, 9, 10) are 1.7 mm**, not the library's
  2.0 mm, for 0.84 mm between them instead of 0.54 mm (1.0 mm holes, as Omron
  specifies). DRC lists K1 as differing from the library; that's this change.
- D3 straight across C1; Rbl1-3 beside it.
- **R32 sits at C1:** only its 3 V tap (AIN1) runs to U4, so no HV enters the
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

- Each divider's low end sits at U4 (R19/R17 for AIN0, R33 for AIN1), so the
  long runs are the 3 V taps.
- **C18 and C19** (100 nF, AIN1 and AIN0 to GND) go at U4: the taps are about 30 k and
  8.5 k source impedance, and the caps make them low-impedance to noise. The readings are
  slow, so the delay costs nothing. AIN2 (ISENSE) has no cap, because it measures a pulse.

### Still to do on the board

- **U4's footprint must be fixed first.** `Custom:ADS1115 Module` numbers its pads 1-10 in
  header order, but the ADS1115 symbol uses the bare chip's pin numbers, so the module's VDD pad
  gets GND and every other pin is wrong too. Renumber the pads, in header order, to
  **8, 3, 10, 9, 1, 2, 4, 5, 6, 7** (VDD, GND, SCL, SDA, ADDR, ALRT, A0, A1, A2, A3), then
  update the footprint on the board.
- **Update PCB from Schematic** for D14, R44 and J1/J6's new pin order, which moves their nets,
  and for the universal-payphone parts: it removes J5 and adds J10-J14, D15-D19, R49-R53,
  C24 and C25. Place J14 by U3, with Cvcc1 between J14 and U3 pin 7; R53 right at Qa1's
  gate and source (beside Dz1); D16-D19 right at J3/J12, so the
  connector side of each line is short; J13 with R52/C25 by U4; J10 and J11 by J2 (HV class).
  J3's pins now connect through D16-D18 (the NICKEL_IN / DIME_IN / QUARTER_IN nets), so the
  ratsnest from J3 changes.
- **Place what's new:** F1, C20, R40-R43 by J1 and J6, with R40 and R41 closest to the
  connectors; D14 by K1; R44 at U4; C21-C23 per the stage 2 notes above; Cp1 at U3
  pins 1 and 2; and:
  - **HV LED** (R27, R28, D5, D7): beside C1, with D7 where it can be seen with the board
    installed. R27/R28 are HV class.
  - **Debug LEDs** D8-D13 with R34-R39: in a row along an edge, labelled on
    silkscreen 5c / 10c / 25c / FIRE / POL / HV EN. D8-D10 and R34-R36 near
    J3, D11-D13 and R37-R39 near U1.
  - **JP1** (VIO = 5V) near J6, with its label on silkscreen.
- **K1's switch pads to 1.7 mm** (section HV above), if not done yet: until then DRC flags
  four K1 pad pairs at 0.54 mm.
- Silkscreen pin 1 and the signal names at J1 and J6. The v6 end is J11, wired pin for pin.
- Board outline and mounting holes (M3, away from HV).
- Test points: 20 V, VIO, HV (a probe pad behind a series resistor), GND.
- Two reference designators overlap (Cz1/Rz1, Ct1/R18): nudge them when
  tidying silkscreen.
