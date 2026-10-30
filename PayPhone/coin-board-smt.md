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

## BOM: THT (v1) and SMT

| Ref | Function | **THT (v1, DigiKey)** | **SMT option** | SMT package | JLCPCB | Notes |
|---|---|---|---|---|---|---|
| U1 | I2C I/O expander | MCP23008-E/P, DIP-18 ($1.54) | MCP23008-E/SO | SOIC-18 | Extended (verify) | same symbol family (`MCP23008-xSO`) |
| U2 | stage 1, 5 V to 20 V | MC34063AP1G, DIP-8 ($1.98) | MC34063A in SOIC-8 (e.g. MC34063ADR) | SOIC-8 | Extended (verify) | same pinout, same R/C values; `MC34063AD` symbol |
| U3 | stage 2, 20 V to HV | UC3843AN, DIP-8 ($0.34) | UC3843 in SOIC-8 (e.g. UC3843BD1R2G) | SOIC-8 | Extended (verify) | `UC3843_SOIC8` symbol, same pins |
| Qa | stage 2 switch, 200 V | IRF630PBF, TO-220 ($2.43) | keep the TO-220, or a 200 V DPAK (IRFR220N class) | TO-220 / DPAK | Extended | peak currents near 1 A: a SOT-223 (1 A) part is too small here |
| Q3 | fire switch, 200 V | IRF630PBF, TO-220 ($2.43) | **STN4NF20L** | SOT-223 | Extended | 1 A, 200 V; the 12 V gate drive (block C) removes its 3 V-threshold problem |
| Q2, Q4, Q5 | small N-FETs | 2N7000, TO-92 | **2N7002** | SOT-23 | C8545 (Basic, verify) | **swap the symbol** (pin order) |
| Q6 | gate-drive PNP | 2N3906, TO-92 | **MMBT3906** | SOT-23 | C2940 (Basic, verify) | swap the symbol |
| (Q2 alt.) | relay driver NPN | 2N3904, TO-92 | MMBT3904 | SOT-23 | C20526 (Basic, verify) | only if the 2N7000 is marginal |
| D1 | stage 1 Schottky | 1N5819, DO-41 | **SS34** (40 V 3 A) | SMA | C8678 (already used on v6) | |
| D2 | stage 2 ultrafast, 200 V | UF4003, DO-41 | ES1D / ES2D (200 V) | SMA | Extended (verify) | |
| D3 | HV clamp | 1.5KE150A, DO-201 (128 V standoff) | **SMBJ130A** (130 V standoff) | SMB | Extended | **SMBJ numbers are the standoff, 1.5KE numbers the breakdown**: SMBJ150A would be 150 V standoff, a different part from 1.5KE150A |
| D4 | relay flyback | 1N4148, DO-35 | 1N4148W | SOD-123 | C81598 (already used on v6) | |
| D6, Dz | gate zeners 12 V, 15 V | 1N4742A, 1N4744A, DO-41 | BZT52C12, BZT52C15 | SOD-123 | (verify) | |
| C1 | HV storage 220 uF / 200 V | Nichicon UCS2D221MHD ($3.08) | **stays through-hole** | radial 18 mm | hand-solder | no practical SMD part at 200 V / 220 uF |
| C7, C9, C14, C15 | 20 V rail bulk, 22 uF | 22 uF 35 V electrolytic | 22 uF **50 V** 1206 MLCC, or 2 x 10 uF 50 V 1206 | 1206 | Extended (verify) | see "22 uF at 20 V" below |
| C5 | 5 V input bulk, 22 uF | electrolytic | 22 uF 25 V 0805 X5R | 0805 | **C45783** (Basic, verify) | fine at 5 V |
| L1 | stage 1, 100 uH, 2 A+ | radial (to choose) | SMD power inductor (e.g. 12 x 12 mm class) | SMD | Extended | pick Isat >= 2 A |
| L2 | stage 2, 220 uH, 1 A | Bourns RLB9012-221KL ($0.66) | SMD power inductor, 220 uH, Isat >= 1 A | SMD | Extended | |
| K1 | DPDT relay | Omron G6S-2-DC5 ($4.52) | G6S-2F (SMD version) or keep THT | SMD / THT | Extended | 220 VDC contacts either way |
| R11 | stage 1 current sense 0.33 ohm, 1/2 W | axial | 1206 or 2010, 1/2 W | 1206 | (verify) | |
| Rcs | stage 2 current sense 2 ohm, 1 W | axial | 2512, 1 W | 2512 | (verify) | |
| R25 | fire current sense 10 ohm, 1/2 W | axial | 1206, 1/4-1/2 W | 1206 | (verify) | |
| Rbleed, Rfbt, ADC divider | HV resistor chains | axial 1/4 W | **1206** (or 0805, see below) | 1206 | Basic values exist (verify) | 3 in series so each sees under 45 V |
| RV1 | trimmer | Bourns 3296W | keep THT, or SMD 3314J | | Extended | |
| J1, J2, J3, J6-J9 | connectors, jumpers | JST EH, Molex KK 396 (WM4621-ND, WM4622-ND), pin headers | stay through-hole | THT | hand-solder / JLC THT | |
| (debug) | ADS1115 ADC | **ADS1115 module, AliExpress, about $1.90** ([listing](https://www.aliexpress.us/item/2251832488241448.html)) on a 1 x 10 header | ADS1115IDGSR (VSSOP-10, $5.28) or MCP3221 (SOT-23-5, $1.91, 1 channel) | | | optional; fit only on bring-up boards |

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
