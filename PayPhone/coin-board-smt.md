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
| U3 | stage 2 controller | **UC3843BNG**, DIP-8 | **1.87** | UC3843BD1R2G, SOIC-8 | 0.71 | Extended | hand-solder. **Only 56 in stock** at DigiKey for the SOIC; 33-week lead time |
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
| L1 | 100 uH, 2 A | radial (to choose) | ~1 | Bourns SRR1260-101M class, 12 x 12 mm | ~1.05 (est.) | Extended | hand-solder |
| L2 | 220 uH, 1 A | RLB9012-221KL | 0.66 | Bourns SRR1260-221K / Murata 48221SC | 1.05 / 1.14 | Extended | hand-solder |
| K1 | DPDT relay | G6S-2-DC5 | 4.52 | G6S-2F-DC5 | 5.08 | Extended | keep through-hole (cheaper, sturdier) |
| R, C | 0805 resistors/caps | axial / disc | | 0805 | | Basic for common values | **JLCPCB places** |
| R11, Rcs, R26 | power resistors | axial | | 1206 / 2512 | | check | Basic values exist for some |
| C7, C9, C14, C15 | 20 V rail bulk | 22 uF 35 V electrolytic | | 22 uF 50 V 1206 or 2 x 10 uF 50 V 1206 | not found / ~0.3 | Extended (verify) | hand-solder, or keep SMD electrolytics |
| J1-J3, J6-J9, RV1 | connectors, trimmer | | | stay through-hole | | n/a | hand-solder |
| U4 | ADS1115 (debug only) | AliExpress module, ~1.90 ([listing](https://www.aliexpress.us/item/2251832488241448.html)) | 1.90 | ADS1115IDGSR, VSSOP-10 | 5.28 | Extended | leave unfitted; module for bring-up |

**Per board, the semiconductors** (U1-U3, Q2-Q6, D1-D6) come to about
**$11 through-hole vs. about $5 SMT** at these prices.

### Is the $3 Extended fee worth it?

The fee is **per unique Extended part, per order**, not per board, so it
shrinks with order size:

| Boards in the order | Fee per Extended part, per board |
|---:|---:|
| 5 | $0.60 |
| 10 | $0.30 |
| 30 | $0.10 |

With about 8 Extended parts here (U1, U2, U3, Q3, D2, D3, L1, L2), that's
**$24 per order**: $4.80 a board at 5 boards, $0.80 at 30.

**Recommendation**

- **First SMT order (5 boards): JLCPCB places only Basic parts**, which are free
  to place: the 2N7002s, MMBT3906, SS34, 1N4148W, zeners, and the 0805 R/C.
  You hand-solder the Extended ones. They're all easy packages (SOIC-8/18,
  SOT-223, SMA, SMB, 12 mm inductors), only one of each per board, and
  hand-soldering them saves about $24 an order, nearly the whole saving from
  going SMT.
- **30 boards or more: let JLCPCB place everything SMT.** The fees drop to
  about $0.80 a board, less than your time.
- **Through-hole on every version:** C1, K1, Qa (TO-220), the connectors
  and jumpers, RV1. JLCPCB's through-hole service costs extra too, and
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
