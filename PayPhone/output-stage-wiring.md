# Coin relay output stage: what connects where

**In progress: not for use or fab.** The PCB layout isn't finished, and nothing here is built
or tested.

Reference designators and pin numbers match
`Payphone_v1/Payphone_v1.kicad_sch`.

The output stage takes the high voltage on net **HV** (C1, 52-119 V, set by
J7-J9) and sends it through the payphone's coin relay one way or the other.
Three small blocks, each driven by one TCA9534A pin:

| Block | Driven by | Does |
|---|---|---|
| A. Relay driver | **POL** (U1 P5, pin 10) | energizes K1's 5 V coil |
| B. Polarity relay + coil connector | K1's contacts | picks which coin-relay wire gets HV |
| C. Firing switch | **FIRE** (U1 P6, pin 11) | lets current flow through the coin relay |

## The whole stage at a glance

```
                                   HV  (C1 +, 52-119 V)
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
   J2 pin 2 (red): no connect   Q3 pin 2 (D)              STN4NF20L
                                Q3 pin 3 (S)
                                |
                               R26 10 ohm  (current sense)
                                |
                               GND

   RETURN --- D14 anode, D14 cathode --- HV                  (ES2D: the coil's freewheel path)
   +5 V --- K1 pin 1 (coil) ... K1 pin 12 --- Q2 collector   (block A)
   FIRE --- Q4 --- Q5 (from 20V) --- Q3 gate, clamped to 12 V by D6   (block C)
```

**Which way the coin goes** (from the bench tests: + on white moved the lever
left = collect; + on red or green moved it right = return):

| POL (P5) | K1 | J2 pin 1, white | J2 pin 3, green | Coin relay does |
|---|---|---|---|---|
| low (default at power-up) | off | HV | RETURN | **collect** (lever left) |
| high | on | RETURN | HV | **return to caller** (lever right) |

Nothing flows until FIRE goes high. **Change POL only while FIRE is low**, so
K1's contacts never switch a live current.

## Block A: relay driver (POL -> K1 coil)

```
            +5V
             |
      +------+------+
      |             |
  K1 pin 1          D4 pin 1 (K, band)     1N4148W
  [K1 coil]         |
  K1 pin 12 --------+-- D4 pin 2 (A)
      |
  Q2 pin 3 (C)                        MMBT3904
  Q2 pin 1 (B) --- R20 1k --- POL --- U1 pin 10 (P5)
  Q2 pin 2 (E)                 |
      |                        R2 100k
     GND                       |
                              GND
```

| Part | Pin | Connects to |
|---|---|---|
| K1 G6S-2 | 1 (coil) | +5 V |
| K1 | 12 (coil) | Q2 pin 3 (C), D4 pin 2 (A) |
| D4 1N4148W | 1 (K, the band) | +5 V |
| D4 | 2 (A) | K1 pin 12 / Q2 collector |
| Q2 MMBT3904 | 3 (C) | K1 pin 12 |
| Q2 | 1 (B) | R20 to POL |
| Q2 | 2 (E) | GND |
| R20 1k | | Q2 base to POL: about 2.6 mA at VIO = 3.3 V, plenty for the coil's 30-40 mA |
| R2 100k | | POL to GND: keeps K1 off while U1 powers up |

## Block B: polarity relay and the coin-relay connector

K1 pins, from KiCad's G6S-2 symbol (the arm of each switch rests on pins 3
and 10 when the coil is off):

| K1 pin | What it is | Connects to (net) |
|---|---|---|
| 1 | coil | +5 V (block A) |
| 12 | coil | Q2 collector (block A) |
| **4** | COM1 | **COIL_A** -> J2 pin 1 |
| **3** | NC1 (closed when off) | **HV** |
| **5** | NO1 (closed when on) | **RETURN** |
| **9** | COM2 | **COIL_B** -> J2 pin 3 |
| **10** | NC2 (closed when off) | **RETURN** |
| **8** | NO2 (closed when on) | **HV** |

J2 takes the coin relay's own 3-wire cable. The coin switches are a separate
4-wire cable on J3, with their own red and green wires.

| J2 (Molex KK 396, 3 pin) or J11 (screw terminal) | Wire from the coin relay | Net |
|---|---|---|
| 1 | white | COIL_A |
| 2 | red (the relay switch's other side) | not connected |
| 3 | green | COIL_B |

**J11** (screw terminal) has COIL_A on pin 1 and COIL_B on pin 3, like J2, and **COIL_C on
pin 2**, which only J10 connects: its shunt ties COIL_C to HV, for a relay with two coils
and a common wire. J2 pin 2 isn't connected, so the HD550002C's red wire never sees HV.
Which coil fires for each POL setting: coin-board-schematic.md, section 7.

**Check the original plug before routing:** look at which position white, red
and green sit in on the phone's 3-pin plug, and number J2 to match. If the order
differs, only J2's pin numbers change, not the rest.

The coin relay's own switch (red-green) cuts the current once the lever has
moved, which is why the green wire is used: each fire is a single shot.

## Block C: firing switch (FIRE -> Q3)

Q3 is an STN4NF20L (200 V, SOT-223). Q4 and a PNP (Q5) lift FIRE to the 20 V rail,
and D6 clamps the gate at 12 V, so Q3 gets the same full gate drive whatever VIO is.

```
   20V ----+------------------+
           |                  |
          R23 10k          Q5 pin 2 (E)            MMBT3906
           |                  |
           +---- Q5 pin 1 (B) |
           |                  |
          R22 4.7k         Q5 pin 3 (C)
           |                  |
     Q4 pin 3 (D)            R24 1k
     Q4 pin 1 (G) --- R21 1k --- FIRE --- U1 pin 11 (P6)
     Q4 pin 2 (S)             |     |
           |                  |     R3 10k
          GND                 |     |
                              |    GND
                  GATE3 ------+------+---------------+
                              |      |               |
                    Q3 pin 1 (G)   D6 pin 1 (K)    R25 10k
                                   D6 pin 2 (A)      |
                                     |              GND
                                    GND

   Q3 pin 2 (D) = RETURN (block B)
   Q3 pin 3 (S) --- R26 10 ohm --- GND      (net ISENSE between them)
```

| Part | Pin | Connects to |
|---|---|---|
| Q4 2N7002 | 1 (G) | R21 to FIRE |
| Q4 | 2 (S) | GND |
| Q4 | 3 (D) | R22 to Q5 base |
| Q5 MMBT3906 | 2 (E) | 20V |
| Q5 | 1 (B) | R22 (to Q4 drain) and R23 (to 20V) |
| Q5 | 3 (C) | R24 to GATE3 |
| Q3 STN4NF20L | 1 (G) | GATE3 |
| Q3 | 2 (D) | RETURN |
| Q3 | 3 (S) | R26 to GND |
| D6 BZT52C12 | 1 (K, band) | GATE3 |
| D6 | 2 (A) | GND |
| R21 1k | | FIRE to Q4 gate |
| R3 10k | | FIRE to GND: Q3 stays off while U1 powers up |
| R22 4.7k | | Q4 drain to Q5 base (about 4 mA base current) |
| R23 10k | | Q5 base to 20V: turns Q5 off |
| R24 1k | | Q5 collector to GATE3: limits zener current to about 8 mA |
| R25 10k | | GATE3 to GND: turns Q3 off |
| R26 10 ohm (1206) | | Q3 source to GND: current sense (about 1 V at 100 mA) |
| R44 1 k | | ISENSE to U4 AIN2: limits the current into U4 in a coil-short fault |
| D14 ES2D | 1 (K, band) | HV |
| D14 | 2 (A) | RETURN: freewheel path if Q3 turns off while current flows |

How it behaves:

| FIRE | Q4 | Q5 | Q3 gate | Coin relay |
|---|---|---|---|---|
| low, or U1 powering up (R3 holds it low) | off | off | 0 V (R25) | no current |
| high | on | on | 12 V (D6) | current flows HV -> K1 -> coin relay -> K1 -> Q3 -> GND |

**FIRE with EN_HV low.** The 20V rail then sits at about 4.6 V (5 V coming through
L1 and D1) and U3 stays off. That's enough to turn on Q3 **[verify its threshold]**,
so FIRE high would still fire the relay from whatever C1 holds, until C1 has bled
down (D7 out). Firmware must keep FIRE low unless it means to fire.

## Net labels

The HV net class in `Payphone_v1.kicad_pro` matches `/HV*`, `HV*`, `/COIL_A`,
`/COIL_B`, `/COIL_C`, `/RETURN` and `/SW2`, so keep these names: the 1.0 mm clearance then
applies automatically. (A logic net whose name starts with HV would pick up the HV class too,
so don't name one that way.)

| Label | Where |
|---|---|
| HV | C1+, C21 pin 1, D2, D3, D14 cathode, K1 pins 3 and 8, R27, R32, Rbl1, Rfbt1 |
| HV_B1, HV_B2 | between Rbl1-3 (bleed) and between C21-C23 |
| HV_F1, HV_F2 | between Rfbt1-3 (feedback) |
| HV_LED1, HV_LED2 | R27, R28, D5 (HV indicator) |
| SW2 | Qa1 drain, L2, D2 anode: swings up to HV |
| COIL_A | K1 pin 4, J2 pin 1, J11 pin 1 |
| COIL_B | K1 pin 9, J2 pin 3, J11 pin 3 |
| COIL_C | J11 pin 2, J10 pin 2 (J10 pin 1 is HV) |
| RETURN | K1 pins 5 and 10, Q3 pin 2, D14 anode |
| GATE3 | Q3 pin 1, D6 pin 1, R24, R25 |
| RLY_COIL, Q2_B, Q4_G, Q4_D, Q5_B, Q5_C | the small nodes in blocks A and C |
| ISENSE | Q3 pin 3, R26, R44 |
| ISENSE_ADC | R44, U4 AIN2 |

## HV safety

- **Bleed time.** The feedback divider (about 310 k), R32 + R33 (about 1 M), Rbl1-3
  (3 x 1 M) and the HV LED's string (below) drain C1: 119 V falls below 24 V (D7 out)
  in about 33 s and below 20 V in about 42 s. Without the LED string it's about 86 s
  to 20 V; if the divider opens, about 88 s. Still treat C1 as live until D7 is out,
  or short it through a resistor before touching.
- **HV indicator (D7).** HV -> R27 33 k -> R28 33 k -> D5 (BZT52C22, cathode to R28) ->
  D7 red 0805 LED -> GND. It lights above about 24 V (22 V zener + LED). With EN_HV low
  and C1 drained, HV idles at about 4 V, so D7 is dark; it lights whenever C1 is charged.
  It runs from C1 itself, so it stays lit while C1 drains, even with the board unpowered.
  Current: 0.4 mA at 52 V (dim but visible indoors), 1.4 mA at 119 V. R27/R28 share the
  voltage (about 70 mW each at 119 V, so 1206). For a brighter LED at 52 V, use 2 x 22 k
  (0.6 mA at 52 V, 2.2 mA and about 100 mW each at 119 V); that also speeds up the bleed.
- **D3 orientation.** D3 (SMBJ130A) is wired pin 1 to HV and pin 2 to GND. It's
  unidirectional, so the **cathode (band) must be on HV**; check the footprint's pad 1 is
  the cathode before ordering.

## The ADS1115 (U4)

| U4 pin | Connects to |
|---|---|
| 8 VDD | **VIO**, with C17 100 nF (not +5 V: at 5 V the chip wants I2C highs above 3.5 V, more than a 3.3 V bus gives) |
| 3 GND | GND |
| 1 ADDR | GND: address 0x48 |
| 10 SCL, 9 SDA | SCL, SDA (R40 and R41 from the cable) |
| 2 ALERT/RDY | not connected |
| 4 AIN0 | HV through R32 1 M / R33 27 k: 119 V reads 3.13 V. C18 100 nF to GND |
| 5 AIN1 | 20V through R17 56 k / R19 10 k: 20 V reads 3.0 V. C19 100 nF to GND |
| 6 AIN2 | ISENSE (R26) through R44 1 k: 100 mA reads 1 V |
| 7 AIN3 | J13 pin 2 through R52 10 k, C25 100 nF to GND: spare analog input |
