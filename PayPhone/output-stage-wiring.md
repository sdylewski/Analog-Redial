# Coin relay output stage: what connects where

Payphone_v1 (`PayPhone/Payphone_v1/`). Written 2026-10-30 against the schematic
as it was saved then: U1-U3, Qa, Q2-Q5, K1, D1-D6, J1-J9, C1 and the R/C parts
already placed. Reference designators below are the ones in that schematic;
**new parts this stage still needs are marked NEW** with a suggested
reference (R19-R25 and Q6 were free).

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
                               R25 10 ohm 1/2 W  (NEW, current sense)
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
   K1 pin 1      D4 pin 1 (K, band)        1N4148
   [K1 coil]        |
   K1 pin 12 -------+--- D4 pin 2 (A)
      |
   Q2 pin 3 (D)                            2N7000
   Q2 pin 2 (G) ---- R19 1k (NEW) ---- POL ---- U1 pin 15 (GP5)
   Q2 pin 1 (S)                               |
      |                                     R2 100k (already there)
     GND                                       |
                                              GND
```

| Part | Pin | Connects to |
|---|---|---|
| K1 G6S-2 | 1 (coil) | +5 V |
| K1 | 12 (coil) | Q2 pin 3 (D), D4 pin 2 (A) |
| D4 1N4148 | 1 (K, the band) | +5 V |
| D4 | 2 (A) | K1 pin 12 / Q2 drain |
| Q2 2N7000 | 3 (D) | K1 pin 12 |
| Q2 | 2 (G) | R19 to POL |
| Q2 | 1 (S) | GND |
| **R19 1k (NEW)** | | Q2 gate to POL |
| R2 100k (exists) | | POL to GND: keeps K1 off while the MCP23008 resets |

Note: the G6S-2's 5 V coil draws roughly 30-40 mA. A 2N7000 from a 3.3 V gate
usually handles that, but its worst-case threshold is about 3 V, so a bad one
could leave K1 half-on. If K1 doesn't click reliably, swap Q2 for a **2N3904**
NPN (`Transistor_BJT:2N3904`, same TO-92 footprint; its pins are 1 E, 2 B,
3 C: E to GND, B to R19, C to K1 pin 12). R19 then works as the base resistor.

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
          R22 10k (NEW)    Q6 pin 1 (E)            2N3906 PNP (NEW)
           |                  |
           +---- Q6 pin 2 (B) |
           |                  |
          R21 4.7k (NEW)   Q6 pin 3 (C)
           |                  |
     Q4 pin 3 (D)            R23 1k (NEW)
     Q4 pin 2 (G) --- R20 1k (NEW) --- FIRE --- U1 pin 16 (GP6)
     Q4 pin 1 (S)             |                 |
           |                  |                R3 10k (exists)
          GND                 |                 |
                              |                GND
                  GATE3 ------+------+---------------+
                              |      |               |
                    Q3 pin 1 (G)   D6 pin 1 (K)    R24 10k (NEW)
                                   D6 pin 2 (A)      |
                                     |              GND
                                    GND

   Q3 pin 2 (D) = RETURN (block B)
   Q3 pin 3 (S) --- R25 10 ohm 1/2 W (NEW) --- GND      (net ISENSE between them)
```

| Part | Pin | Connects to |
|---|---|---|
| Q4 2N7000 | 2 (G) | R20 to FIRE |
| Q4 | 1 (S) | GND |
| Q4 | 3 (D) | R21 to Q6 base |
| **Q6 2N3906 (NEW)** `Transistor_BJT:2N3906`, `TO-92_Inline` | 1 (E) | 20V |
| Q6 | 2 (B) | R21 (to Q4 drain) and R22 (to 20V) |
| Q6 | 3 (C) | R23 to GATE3 |
| Q3 IRF630 | 1 (G) | GATE3 |
| Q3 | 2 (D) | RETURN |
| Q3 | 3 (S) | R25 to GND |
| D6 12 V zener (1N4742A) | 1 (K, band) | GATE3 |
| D6 | 2 (A) | GND |
| **R20 1k (NEW)** | | FIRE to Q4 gate |
| R3 10k (exists) | | FIRE to GND: Q3 stays off through a reset |
| **R21 4.7k (NEW)** | | Q4 drain to Q6 base (about 4 mA base current) |
| **R22 10k (NEW)** | | Q6 base to 20V: turns Q6 off |
| **R23 1k (NEW)** | | Q6 collector to GATE3: limits zener current to about 8 mA |
| **R24 10k (NEW)** | | GATE3 to GND: turns Q3 off |
| **R25 10 ohm, 1/2 W (NEW)** | | Q3 source to GND: current sense (about 1 V at 100 mA); use a wire link if you skip the ADC |

How it behaves:

| FIRE | Q4 | Q6 | Q3 gate | Coin relay |
|---|---|---|---|---|
| low, or U1 resetting (R3 holds it low) | off | off | 0 V (R24) | no current |
| high | on | on | 12 V (D6) | current flows HV -> K1 -> coin relay -> K1 -> Q3 -> GND |

When EN_HV is low the 20V rail sits at about 4.6 V (5 V coming through L1
and D1) and the UC3843 stays off, so even a stray FIRE can't push current.

## Net labels to add

Use these exact label names; the HV net class in `Payphone_v1.kicad_pro`
already matches `/HV*`, `/COIL_A`, `/COIL_B`, `/RETURN` and `/SW2`, so the
1.0 mm clearance applies automatically.

| Label | Where |
|---|---|
| HV | already on C1+ / D2 / D3 / Rbleed1 / Rfbt1; add K1 pins 3 and 8 |
| COIL_A | K1 pin 4, J2 pin 1 |
| COIL_B | K1 pin 9, J2 pin 3 |
| RETURN | K1 pins 5 and 10, Q3 pin 2 |
| GATE3 | Q3 pin 1, D6 pin 1, R23, R24 (optional label, low voltage) |
| ISENSE | Q3 pin 3, R25 (optional; goes to the ADC) |
| **SW2** | the net now called `Net-(D2-A)`: Qa drain, L2, D2 anode. It swings up to HV, so it belongs in the HV class |

Two more nets carry a large share of HV but have auto names, so the HV class
misses them: `Net-(Rbleed1-Pad1)` and `Net-(Rbleed2-Pad1)` (about 2/3 and
1/3 of HV), and `Net-(Rfbt1-Pad1)`, `Net-(Rfbt2-Pad1)`. Label them (for example
HV_B1, HV_B2, HV_F1, HV_F2) and the `/HV*` pattern picks them up.

## Things I noticed on the rest of the sheet

- **Bleed time.** Rbleed1-3 are 3 x 1 M, so the bleed alone gives
  3 M x 220 uF = about 660 s (11 min). The feedback chain (Rfbt 300 k plus
  the bottom resistors) also drains C1, which brings it to roughly a minute
  for the voltage to fall to a third. That's slow to wait for before touching
  the board. 3 x 33 k (about 100 k) would make it about 20 s, at about 0.17 W
  while charged at 130 V (so 1/4 W parts, 3 in series: each sees under 0.06 W).
- **D3 orientation.** D3 (1.5KE150A) is wired pin 1 to HV and pin 2 to GND.
  For the unidirectional part the **cathode (band) must be on HV**; check
  the footprint's pad 1 is the cathode before ordering. Its standoff is
  128 V, so don't set the jumpers above about 128 V.
- **J7-J9 settings** (UC3843 reference 2.5 V, Rfbt total 300 k, Rfbb4 15 k
  always fitted): none fitted about 52 V; J9 (100 k) about 60 V; J8 (30 k)
  about 78 V; J7 (22 k) about 87 V; all three about 119 V. Worth a silkscreen
  table next to the jumpers.

## Optional: the ADS1115 debug module

For bring-up only (the module is under $2 on AliExpress; see the BOM). Fit a
1 x 10 header for it and leave it empty on production boards.

| Module pin (typical blue module: check yours) | Connects to |
|---|---|
| VDD | +3V3 |
| GND | GND |
| SCL | SCL |
| SDA | SDA |
| ADDR | leave open (the module pulls it to GND: address 0x48) |
| ALRT | no connect |
| A0 | HV divider tap: HV -> 3 x 390 k (NEW) -> tap -> 30 k (NEW) -> GND, 100 nF across the 30 k. 130 V reads about 3.25 V |
| A1 | ISENSE (R25): 100 mA reads 1 V |
| A2 | 20V divider tap: 20V -> 56 k (NEW) -> tap -> 10 k (NEW) -> GND: 20 V reads about 3.0 V |
| A3 | spare |

Most of these modules also carry 10 k I2C pull-ups; in parallel with the main
board's 4.7 k that's about 3.2 k, which is fine.
