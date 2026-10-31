# Coin relay output stage: what connects where

Payphone_v1 (`PayPhone/Payphone_v1/`). Written 2026-10-30 against the schematic
as it was saved then: U1-U3, Qa, Q2-Q5, K1, D1-D6, J1-J9, C1 and the R/C parts
already placed. Reference designators below are the ones in that schematic;
parts this stage added are marked NEW. **They are now in the schematic** (2026-10-30): Q6,
R20-R26 and C16/C17, connected with net labels, checked against a netlist export.

The output stage takes the high voltage on net **HV** (C1, 52-120 V, set by
J7-J9) and sends it through the payphone's coin relay one way or the other.
Three small blocks, each driven by one MCP23008 pin:

| Block | Driven by | Does |
|---|---|---|
| A. Relay driver | **POL** (U1 GP5, pin 15) | energizes K1's 5 V coil |
| B. Polarity relay + coil connector | K1's contacts | picks which coin-relay wire gets HV |
| C. Firing switch | **FIRE** (U1 GP6, pin 16) | lets current flow through the coin relay |

## The whole stage at a glance

```
                                   HV  (C1 +, 52-120 V)
                                    |
                     +--------------+--------------+
                     |                             |
                K1 pin 3 (NC1)                K1 pin 8 (NO2)
                     \                             \
      COIL_A <--- K1 pin 4 (COM1)       COIL_B <--- K1 pin 9 (COM2)
        |            /                    |            /
        |       K1 pin 5 (NO1)            |       K1 pin 10 (NC2)
        |            |                    |            |
        |            +----------+---------+------------+
        |                       |
   J2 pin 1 (white)          RETURN
   J2 pin 3 (green)             |
   J2 pin 2 (red): no connect   Q3 drain (pin 2)          IRF630
                                Q3 source (pin 3)
                                |
                               R26 10 ohm 1/2 W  (NEW, current sense)
                                |
                               GND

   +5 V --- K1 pin 1 (coil) ... K1 pin 12 --- Q2 drain     (block A)
   FIRE --- Q4 --- Q6 (from 20V) --- Q3 gate, clamped to 12 V by D6   (block C)
```

**Which way the coin goes** (from the bench tests: + on white moved the lever
left = collect; + on red or green moved it right = return):

| POL (GP5) | K1 | J2 pin 1, white | J2 pin 3, green | Coin relay does |
|---|---|---|---|---|
| low (default at reset) | off | HV | RETURN | **collect** (lever left) |
| high | on | RETURN | HV | **return to caller** (lever right) |

Nothing flows until FIRE goes high. **Change POL only while FIRE is low**, so
K1's contacts never switch a live current (they only ever switch "dead").

## Block A: relay driver (POL -> K1 coil)

```
            +5V
             |
      +------+------+
      |             |
  K1 pin 1          D4 pin 1 (K, band)     1N4148
  [K1 coil]         |
  K1 pin 12 --------+-- D4 pin 2 (A)
      |
  Q2 pin 3 (D)                        2N7000
  Q2 pin 2 (G) --- R20 1k --- POL --- U1 pin 15 (GP5)
  Q2 pin 1 (S)                 |
      |                        R2 100k (already there)
     GND                       |
                              GND
```

| Part | Pin | Connects to |
|---|---|---|
| K1 G6S-2 | 1 (coil) | +5 V |
| K1 | 12 (coil) | Q2 pin 3 (D), D4 pin 2 (A) |
| D4 1N4148 | 1 (K, the band) | +5 V |
| D4 | 2 (A) | K1 pin 12 / Q2 drain |
| Q2 2N7000 | 3 (D) | K1 pin 12 |
| Q2 | 2 (G) | R20 to POL |
| Q2 | 1 (S) | GND |
| **R20 1k (NEW)** | | Q2 gate to POL |
| R2 100k (exists) | | POL to GND: keeps K1 off while the MCP23008 resets |

Note: the G6S-2's 5 V coil draws roughly 30-40 mA. A 2N7000 from a 3.3 V gate
usually handles that, but its worst-case threshold is about 3 V, so a bad one
could leave K1 half-on. If K1 doesn't click reliably, swap Q2 for a **2N3904**
NPN (`Transistor_BJT:2N3904`, same TO-92 footprint; its pins are 1 E, 2 B,
3 C: E to GND, B to R20, C to K1 pin 12). R20 then works as the base resistor.

## Block B: polarity relay and the coin-relay connector

K1 pins, from KiCad's G6S-2 symbol (the arm of each switch rests on pins 3
and 10 when the coil is off):

| K1 pin | What it is | Connects to (net) |
|---|---|---|
| 1 | coil | +5 V (block A) |
| 12 | coil | Q2 drain (block A) |
| **4** | COM1 | **COIL_A** -> J2 pin 1 |
| **3** | NC1 (closed when off) | **HV** |
| **5** | NO1 (closed when on) | **RETURN** |
| **9** | COM2 | **COIL_B** -> J2 pin 3 |
| **10** | NC2 (closed when off) | **RETURN** |
| **8** | NO2 (closed when on) | **HV** |

| J2 (Molex KK 396, 3 pin) | Wire in the payphone | Net |
|---|---|---|
| 1 | white | COIL_A |
| 2 | red (the coin switch's other side) | **no connect** (put a no-connect flag on it) |
| 3 | green | COIL_B |

**Check the original plug before routing:** look at which position white,
red and green sit in on the phone's 3-pin plug, and number J2 to match. If
the order differs, only J2's pin numbers change, not the rest.

The coin relay's own switch (red-green) cuts the current once the lever has
moved, which is why the green wire is used: each fire is a single shot.

## Block C: firing switch (FIRE -> Q3)

Q3 is an IRF630, which needs about 10 V on its gate, more than the
MCP23008's 3.3 V. Q4 and a PNP (Q6) lift FIRE to the 20 V rail, and D6
clamps the gate at 12 V.

```
   20V ----+------------------+
           |                  |
          R23 10k (NEW)    Q6 pin 1 (E)            2N3906 PNP (NEW)
           |                  |
           +---- Q6 pin 2 (B) |
           |                  |
          R22 4.7k (NEW)   Q6 pin 3 (C)
           |                  |
     Q4 pin 3 (D)            R24 1k (NEW)
     Q4 pin 2 (G) --- R21 1k (NEW) --- FIRE --- U1 pin 16 (GP6)
     Q4 pin 1 (S)             |          |
           |                  |          R3 10k (exists)
          GND                 |          |
                              |         GND
                  GATE3 ------+------+---------------+
                              |      |               |
                    Q3 pin 1 (G)   D6 pin 1 (K)    R25 10k (NEW)
                                   D6 pin 2 (A)      |
                                     |              GND
                                    GND

   Q3 pin 2 (D) = RETURN (block B)
   Q3 pin 3 (S) --- R26 10 ohm 1/2 W (NEW) --- GND      (net ISENSE between them)
```

| Part | Pin | Connects to |
|---|---|---|
| Q4 2N7000 | 2 (G) | R21 to FIRE |
| Q4 | 1 (S) | GND |
| Q4 | 3 (D) | R22 to Q6 base |
| **Q6 2N3906 (NEW)** `Transistor_BJT:2N3906`, `TO-92_Inline` | 1 (E) | 20V |
| Q6 | 2 (B) | R22 (to Q4 drain) and R23 (to 20V) |
| Q6 | 3 (C) | R24 to GATE3 |
| Q3 IRF630 | 1 (G) | GATE3 |
| Q3 | 2 (D) | RETURN |
| Q3 | 3 (S) | R26 to GND |
| D6 12 V zener (1N4742A) | 1 (K, band) | GATE3 |
| D6 | 2 (A) | GND |
| **R21 1k (NEW)** | | FIRE to Q4 gate |
| R3 10k (exists) | | FIRE to GND: Q3 stays off through a reset |
| **R22 4.7k (NEW)** | | Q4 drain to Q6 base (about 4 mA base current) |
| **R23 10k (NEW)** | | Q6 base to 20V: turns Q6 off |
| **R24 1k (NEW)** | | Q6 collector to GATE3: limits zener current to about 8 mA |
| **R25 10k (NEW)** | | GATE3 to GND: turns Q3 off |
| **R26 10 ohm, 1/2 W (NEW)** | | Q3 source to GND: current sense (about 1 V at 100 mA); use a wire link if you skip the ADC |

How it behaves:

| FIRE | Q4 | Q6 | Q3 gate | Coin relay |
|---|---|---|---|---|
| low, or U1 resetting (R3 holds it low) | off | off | 0 V (R25) | no current |
| high | on | on | 12 V (D6) | current flows HV -> K1 -> coin relay -> K1 -> Q3 -> GND |

When EN_HV is low the 20V rail sits at about 4.6 V (5 V coming through L1
and D1) and the UC3843 stays off, so even a stray FIRE can't push current.

## Net labels (now placed)

These are on the schematic; keep the names, because the HV net class in `Payphone_v1.kicad_pro`
already matches `/HV*`, `/COIL_A`, `/COIL_B`, `/RETURN` and `/SW2`, so the
1.0 mm clearance applies automatically.

| Label | Where |
|---|---|
| HV | already on C1+ / D2 / D3 / Rbleed1 / Rfbt1; add K1 pins 3 and 8 |
| COIL_A | K1 pin 4, J2 pin 1 |
| COIL_B | K1 pin 9, J2 pin 3 |
| RETURN | K1 pins 5 and 10, Q3 pin 2 |
| GATE3 | Q3 pin 1, D6 pin 1, R24, R25 |
| RLY_COIL, Q2_G, Q4_G, Q4_D, Q6_B, Q6_C | the small nodes in blocks A and C, labelled so the parts can sit apart on the sheet |
| ISENSE | Q3 pin 3, R26, U4 AIN2 |
| **SW2** | the net now called `Net-(D2-A)`: Qa drain, L2, D2 anode. It swings up to HV, so it belongs in the HV class |

The resistor-chain midpoints carry 1/3-2/3 of HV, so they're labelled HV_B1, HV_B2
(bleed) and HV_F1, HV_F2 (feedback) and the `/HV*` pattern puts them in the HV class.

## Things I noticed on the rest of the sheet

- **Bleed time.** As designed in coin-board-schematic.md, the feedback divider
  (about 310 k) is C1's main bleed, about 69 s time constant; Rbleed1-3 (3 M)
  are the backup if the divider opens (11 min). Treat C1 as live for a few
  minutes after power-off, or short it through a resistor before touching.
- **D3 orientation.** D3 (1.5KE150A) is wired pin 1 to HV and pin 2 to GND.
  For the unidirectional part the **cathode (band) must be on HV**; check
  the footprint's pad 1 is the cathode before ordering. Its standoff is
  128 V, so don't set the jumpers above about 128 V.
- **J7-J9 settings** (UC3843 reference 2.5 V, Rfbt total 300 k, Rfbb4 15 k
  always fitted): none fitted about 52 V; J9 (100 k) about 60 V; J8 (30 k)
  about 78 V; J7 (22 k) about 87 V; all three about 119 V. Worth a silkscreen
  table next to the jumpers.

## The ADS1115 (U4)

U4 is on the schematic as the chip (ADS1115IDGS); for bring-up you can fit
the under-$2 AliExpress module instead (see the BOM) and leave U4 empty. As
wired now:

| U4 pin | Connects to |
|---|---|
| 8 VDD | **+3V3** (changed from +5 V: at 5 V the chip wants I2C highs above 3.5 V, more than the 3.3 V bus gives), with **C17** 100 nF |
| 3 GND | GND |
| 1 ADDR | **GND** (was floating): address 0x48 |
| 10 SCL, 9 SDA | SCL, SDA |
| 2 ALERT/RDY | no connect |
| 4 AIN0 | HV through R15 1.17 M / R16 33 k: 119 V reads 3.27 V |
| 5 AIN1 | 20V through **R17 56 k / R19 10 k** (was 33 k / 33 k, which put 10 V on the pin and would destroy the ADC): 20 V reads 3.0 V |
| 6 AIN2 | ISENSE (R26): 100 mA reads 1 V |
| 7 AIN3 | spare |

Most ADS1115 modules also carry 10 k I2C pull-ups; with the main board's
4.7 k that's about 3.2 k, which is fine.
