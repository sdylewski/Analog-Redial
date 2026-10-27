# Payphone coin board: schematic and parts (draft 1)

Private, untracked. 2026-10-25. Companion to
[coin-return-board.md](coin-return-board.md) (measurements, patent, converter
analysis). **Nothing here is built or simulated.** Values marked **[verify]**
come from distributor listings or my memory of a datasheet, not from the
manufacturer's datasheet; check them before ordering.

One board that (1) fires the HD550002C coin relay in either direction from
a high-voltage supply made on the board, (2) reads the coin sensor's
5 / 10 / 25 cent lines, and (3) talks to Analog Redial over I2C. It needs
only **5 V, 3.3 V, GND, SDA, SCL** from the main board.

## 1. Block diagram

```
 Analog Redial                         COIN BOARD
 (v6 accessory hdr)                                            HV zone (up to 130 V)
 +5V  --- J1.1 --+-- F1 --+--> [U2 MT3608/SX1308 5V->20V] --> +20V --> [U3 TPS40210 + Q1 + L2 + D2] --> HV+ (C_out)
 +3V3 --- J1.2   |        |         ^EN                              ^DIS/EN                        |
 SDA  --- J1.3 --+        +-> sensor +5V (J3, JP1)                    |                              |
 SCL  --- J1.4 --+--> [U1 MCP23008 @0x20] GP4 HV_EN ----------------+                              |
 GND  --- J1.5      |        GP5 POL --> Q2 --> K1 (DPDT relay) <------ HV+ ------------------------+
 (J1 = main board v6 J6)|  GP6 FIRE --> Q3 (200 V FET, low side) --> R_sense --> GND
                    |        GP0..2 <-- coin inputs <-- J3 (coin sensor)
                    |        GP3, GP7: spare, to J5

 K1 poles --> J2 (coil: A / B).  Relay on the coil: HD550002C, white + green wires.
```

## 2. Connectors

| Ref | Pins | Function |
|---|---|---|
| **J1** | 5 x 2.54 mm; mates with the main board's **v6 J6** | **1 +5 V, 2 +3.3 V, 3 SDA, 4 SCL, 5 GND**, the same order as J6, so a straight 1:1 cable. **No INT line** (Analog Redial has no spare GPIO) |
| **J2** | **Molex KK 396, 3-pin header, 0026604030** | the relay's plug: A = white, B = green, red unconnected (so the original plug fits). **Check its voltage rating against 130 V** |
| **J3** | **Molex KK 396, 4-pin header, 0026604040** | coin sensor. **The sensor has 5 wires but this is a 4-pin plug: identify which wires are used**; if five are needed, a 5-pin KK (26-60-4050) is the next size. Intended use: +V, GND, NICKEL, DIME, QUARTER. **The sensor's 5 wires (white, blue, red, green, white) are not yet identified**; measure it first (patent says supply 3-9 V and three open-collector outputs) |
| **J4** | 1 x 4 header, **HV profile select** | one jumper chooses the output voltage (section 5) |
| **J5** | 1 x 4 | GP3, GP7 (spare I/O), 3V3, GND |
| JP1 | solder jumper | sensor supply: +5 V (default) or an external supply on J3 |
| JP2-JP4 | solder jumpers | MCP23008 address A0-A2 (default all open = 0x20) |

## 3. I2C devices and the main board

| Device | Address | Notes |
|---|---|---|
| ES8388 (on the main board) | 0x10 | already there |
| **U1 MCP23008** | **0x20** (A0-A2 = GND; jumpers pull each to 3V3) | VDD = 3.3 V; /RESET tied to 3.3 V; I/O pins default to inputs at power-up, so every output below has a pulldown |

The main board already has the I2C pull-ups; add **DNP** 4.7 k pads on this
board in case it is used standalone.

## 4. Pin assignment (MCP23008, VDD = 3.3 V)

| Pin | Net | Dir | Idle | Notes |
|---|---|---|---|---|
| GP0 | NICKEL | in | high (pull-up to 3V3) | open-collector sensor out, 10 k pull-up, 1 k series, 100 nF to GND |
| GP1 | DIME | in | high | same |
| GP2 | QUARTER | in | high | same |
| GP3 | SPARE_A | in/out | | to J5 |
| GP4 | **HV_EN** | out | **low** (100 k pulldown) | enables U2 and U3: HV exists only while high |
| GP5 | **POL** | out | low (100 k pulldown) | drives Q2 and relay K1: sets which way the coil is driven |
| GP6 | **FIRE** | out | low (10 k pulldown at the gate) | gates Q3: current through the coil |
| GP7 | SPARE_B | in/out | | to J5 |

At reset the MCP23008 is all inputs, the pulldowns hold HV_EN, POL and FIRE
low, so **nothing fires and the HV supply is off** until firmware says so.

## 5. High-voltage supply (two stages)

### Stage 1: 5 V to 20 V, U2 = MT3608 or SX1308 (SOT-23-6) **[verify pinout and EN pin number]**

Both are listed with the same pinout (pin 1 SW, 2 GND, 3 FB, 4 EN, 5 IN, 6 NC, per the SX1308 datasheet excerpt; the MT3608 EN/IN pins weren't in what I found). **Check the SW absolute maximum:** one MT3608L listing shows **22 V**, the SX1308 28 V. With the output at 24 V the SW pin sits at about 24.5 V, so I lowered stage 1 to **20 V**, which is safe for either part.

- IN: +5 V through F1 (about 500 mA PTC; **[verify]** the main board's 5 V
  rail can supply it while charging), C_in = 22 uF / 10 V + 100 nF.
- L1 = 22 uH, Isat >= 2 A. D1 = SS34 (Schottky, 40 V). C_out1 = 2 x 22 uF / 35 V (25 V is enough).
- Feedback: Vout = 0.6 V x (1 + R_top / R_bot): **R_top = 330 k, R_bot = 10 k
  gives 20.4 V**.
- EN: from HV_EN (with a 100 k pulldown). 5 V to 20 V is 4x, about 75 % duty, fine.

### Stage 2: 20 V to 50-130 V, U3 = TPS40210 (boost controller, MSOP-10) **[verify all]**

- Wide input (4.5-52 V), non-synchronous boost, drives a grounded-source
  N-FET, max duty about 95 % and switch current limit about 6 A from distributor
  tables; DIS/EN pin (I could not get its threshold from the data I found, so
  confirm it works from a 3.3 V logic level; otherwise drive it through
  a transistor from 5 V).
- 20 V to 130 V is a ratio of about 6.5x (duty about 85 %), inside the 95 % limit. Check the controller's minimum off time at the chosen frequency.
- **Q1: 200 V N-FET, about 3 A or more, low Qg, in a DPAK-class package.** An
  IRFR220N-class part is the idea. **[verify]** stock at JLCPCB and gate-drive
  compatibility with the controller's gate voltage.
- **L2: about 220 uH, Isat >= 1 A, shielded.** Starting value only: the
  datasheet's sizing equations decide it. Frequency about 100-200 kHz.
- **D2: 200 V ultrafast.** SMD: **ES2D** (**[verify the package]**: distributors list SMA and SMB for the same name). **Through-hole** (axial): **MUR220** (2 A, 200 V, about 35 ns) matches best, but onsemi's datasheet says it is **discontinued, not for new designs**, so check stock; **MUR120** / **UF4003** (1 A, 200 V) and **UF5402** (3 A, 200 V) are similar axial parts I know of but **couldn't verify here**. The average current is only tens of mA, so 1 A is enough.
- **C_out: 220 uF / 200 V**, or 2 x 100 uF / 200 V in parallel (bulky
  radial, about 16 x 25 mm: check the pad fit). Energy at 130 V is about 3.7 J.
  For a pulse of 100 mA for 50 ms (5 mC) the droop is about 23 V
  (dV = Q / C); once the relay pull-in current is measured, resize C.
- **Feedback** (Vref assumed 0.7 V **[verify]**): Vout = 0.7 x (1 + R1 / R2).
  **R1 = 3 x 200 k in series (600 k)** so no single resistor sees more than about 50 V, and a
  duplicate series chain in parallel is **not** needed if the OVP below is fitted.
  R2 chosen by jumper J4:

  | J4 position | R2 (to GND) | Vout (about) |
  |---|---|---|
  | 1 | 3.3 k | **128 V** |
  | 2 | 4.3 k | 98 V |
  | 3 | 6.2 k | 69 V |
  | 4 | 8.2 k | 52 V |
  | none | open | converter regulates to its minimum, **safe** |

  **R1 open or missing** would let the output run away: that is the failure
  to protect against.
- **OVP: a 150 V TVS or zener clamp across C_out** (1.5KE150A / SMBJ150A class has a **128 V standoff**, breakdown about 143-158 V: fine with 128 V as the highest profile **[verify]**) so a feedback fault clamps below
  the 200 V parts' limit, at the cost of a clamp that may overheat if the fault persists.
- **Bleed: 3 x 33 k in series (99 k) across C_out**: time constant 22 s with
  220 uF, about 0.17 W at 130 V. HV also falls when HV_EN goes low.
- **Soft rise:** the converter's soft-start plus the F1 fuse limit input
  current; **[verify]** charging 3.7 J does not pull the 5 V rail below spec.

## 6. Firing and polarity

```
 K1 set 1:  COM1 -> J2.A (white)   NC1 -> HV+      NO1 -> RETURN
 K1 set 2:  COM2 -> J2.B (green)   NC2 -> RETURN   NO2 -> HV+

   relay off: A = HV+,    B = RETURN   (one direction)
   relay on : A = RETURN, B = HV+      (the other direction)

 RETURN = drain of Q3 (200 V logic-level N-FET); Q3 source -> R_sense 10 ohm -> GND
 Current path: HV+ -> K1 -> coil -> K1 -> RETURN -> Q3 -> R_sense -> GND
```

Which direction is return and which is deposit is still to confirm on the
bench (earlier: + on red / white at ground moved the lever right = return).

- **K1: DPDT signal relay, 5 V coil, contacts rated at least 220 V DC.**
  **Omron G6S-2 (5 V)** or **Panasonic TX2-5V** are the candidates: distributor listings show 220 VDC / 2 A, but
  **verify against the datasheet**. The **TQ2-5V** (110 V DC max) is
  **not** enough for the 130 V profile, fine only up to about 100 V.
- Coil of K1: 5 V through **Q2 (NPN or N-FET, e.g. BC817 / 2N7002)** from GP5
  (base resistor 1 k, 100 k pulldown), flyback diode across the coil (1N4148W).
- **Q3: STMicro STN4NF20L** (200 V, 1 A, SOT-223, Rds(on) about 1.1 ohm typ / 1.5 ohm max; the FQT4N20L was obsolete). A distributor lists Vgs(th) max 3 V, so **a 3.3 V gate drive is marginal**: either accept the higher Rds(on) at only about 100 mA, or **level-shift to 5 V** with a two-transistor non-inverting stage (2 x 2N7002, pull-up to +5 V). **[verify Rds(on) vs Vgs on the ST datasheet]**. Gate through 100 ohm, 10 k to GND.
  Current is limited by the relay's own resistor and self-interrupting switch
  (about 50 mA at 60 V measured).
- **Sequence** (firmware): set POL, enable HV_EN, wait until the HV reads
  up to voltage (a fixed delay of about 3 s), pulse FIRE, drop FIRE,
  drop HV_EN. **Never change POL while FIRE is high or while a current flows**,
  so the contacts only switch dead. Cap FIRE at about 1 s in firmware.
- The relay's own switch (red-green wires) cuts the current by itself, so FIRE does not need exact timing.

## 7. Coin inputs

- J3 supply: +5 V through JP1 to the sensor (3-9 V per the patent **[measure]**).
- Each coin line: 10 k pull-up **to 3.3 V** (not to the sensor's supply), 1 k
  series resistor, 100 nF to ground: the MCP23008 input stays within
  3.3 V even if the sensor's supply is higher. Optionally clamp with a BAV99 per line
  (DNP).
- The patent's logic: a nickel blocks one beam, a dime the other, a quarter both.
  The three outputs are separate and open-collector (active low, **[verify]**).
- Firmware polls the MCP23008 about every 2-5 ms and counts edges, with debounce.

## 8. No voltage monitor

The ADS1115 was dropped (it only gave firmware an "HV is up" check and a fire-current trace).
Firmware charges for a **fixed time** (about 3 s, tuned on the bench) and caps FIRE.
If a ready check is wanted later, GP3 is free for an HV-OK comparator.

## 9. Parts list (candidates; stock and part numbers to check on LCSC/JLCPCB)

| Ref | Value | Part | Note |
|---|---|---|---|
| U1 | I/O expander | **MCP23008-E/SO** (SOIC-18) | **[verify]** LCSC stock; address 0x20 |
| U2 | 5 V to 20 V boost | **MT3608** or **SX1308** (SOT-23-6) | **[verify]** pinout and SW max |
| U3 | boost controller | **TPS40210DGQ** (MSOP-10) | **[verify]** EN threshold, Vref, max duty |
| Q1 | 200 V N-FET, >= 3 A | IRFR220N-class (DPAK) | **[verify]** |
| Q2 | relay driver | BC817 or 2N7002 | |
| Q3 | 200 V logic-level N-FET | **STN4NF20L** (SOT-223) | ~~FQT4N20L~~ obsolete; check gate drive |
| K1 | DPDT relay, 5 V coil | **Omron G6S-2** or **Panasonic TX2-5V** | 220 VDC rating **[verify]** |
| D1 | Schottky | SS34 | |
| D2 | 200 V ultrafast | **ES2D** (SMD) or axial **MUR220**-class (through-hole) | **[verify]** package / stock |
| D3 | clamp | 150 V TVS (SMBJ150A) | OVP |
| D4 | relay flyback | 1N4148W | |
| L1 | 22 uH, >= 2 A | | |
| L2 | ~220 uH, >= 1 A | | |
| C_out | 220 uF / 200 V | radial electrolytic | bulky |
| R1 | 3 x 200 k, 0603 or larger | | each sees < 50 V |
| R2 | 3.3 k, 4.3 k, 6.2 k, 8.2 k | | one fitted per J4 position |
| F1 | PTC about 500 mA | | |
| R_sense | 10 ohm, 0.5 W | | |
| J2 | 3-pin KK 396 header | **Molex 0026604030** | mating plug 09-50-3031 + terminals **[verify]** |
| J3 | 4-pin KK 396 header | **Molex 0026604040** | mating plug 09-50-3041 + terminals **[verify]** |
| misc | pull-ups, pulldowns, caps | | |

## 10. Layout notes

- Keep the **HV zone** (C_out, D2, Q1, L2, K1 contacts, Q3, J2, divider
  resistors) together, away from J1 and the logic. **Spacing at least 1.0 mm** for anything that
  can reach 130 V (IPC-2221 external bare copper is about 0.6 mm up to 150 V; use
  more); no logic traces under the HV parts, solder mask is not insulation.
- A **slot or wide gap** between the relay's coil and contact sides.
- Label the HV zone in silkscreen. 130 V DC is just above the 120 V DC extra-low-voltage
  line; the C_out cap stays charged until the bleed runs down, so label it.
- Short, wide switching loop for Q1/L2/D2 and the input capacitor.

## 11. Still to settle

1. **Identify the coin sensor wires** (supply, ground, 3 outputs) and logic levels.
2. **Which phone-side voltage is needed:** the 130 V profile is a margin for other
   phones; this phone works at about 50-60 V. Choose the default J4 position.
3. **Datasheet checks** for the **[verify]** items, above all U3 (enable
   threshold, Vref, gate drive) and K1 (contact rating).
4. **Measure pull-in charge** (current x time at the operating voltage) to size C_out.
5. Firmware: a new MCP23008 driver, coin events, and a coin-relay
   output (return/collect) with a safety timeout. **None of it exists yet.**
6. A KiCad schematic and PCB from this once the above are settled.

## 12. Cable from the main board, and board layout (2026-10-27)

**Plan:** the main board's new I2C header runs by cable to this **daughter
board mounted in the phone**, next to the relay. The 130 V never leaves the
daughter board: the cable carries only 5 V, GND, SDA, SCL. The only wires with
high voltage are the short ones from J2 to the relay.

### I2C over a cable of up to about 1 m

- The limit is bus capacitance (400 pF max for I2C). Twisted pair is roughly
  50-100 pF per metre, plus the board traces and devices, so **1 m is workable
  at 100 kHz** with the main board's pull-ups (4.7 k; rise time about
  0.85 x R x C, around 0.5 us, under the 1 us standard-mode limit). **Don't
  run this bus at 400 kHz** with the long cable (it needs 300 ns). The ES8388
  shares the bus, so set the firmware's I2C clock for the whole bus to
  100 kHz, or buffer the cable run (below).
- **Twist each signal with a ground wire** (SDA+GND, SCL+GND), or use a
  shielded pair, so the cable doesn't pick up the boost converter's switching
  noise. A 4-wire cable: 5 V, GND, SDA, SCL.
- At the daughter-board end: **100 ohm series resistors** in SDA and SCL, a
  small ESD/TVS part on each line (the cable is an antenna for static in a
  phone), and a 100 nF to ground near the MCP23008.
- **Make 3.3 V locally** (an XC6206-class LDO from the 5 V, the part the main
  board already uses) instead of running a 3.3 V wire: no cable drop or noise.
- **Power on the cable:** charging a 130 V capacitor pulls current for a few
  seconds. Use **24 AWG or thicker** for 5 V and GND (or two wires each), and
  put **100 uF near the cable entry** on the daughter board.
- **If the bus misbehaves** (the ES8388 loses its config when the cable is
  touched): add an I2C buffer (TCA4307 or PCA9517 class; I haven't checked
  stock) at the main-board end so the cable can't load the codec's bus, or a
  differential extender such as the PCA9615 for longer runs. **Probably not
  needed under 1 m.**

### Floor plan (2 layers, about 60 x 50 mm to start; the 220 uF / 200 V
capacitor is about 16 x 25 mm and sets the size)

```
 +----------------------------------------------------------------+
 | LOGIC ZONE (left)          | POWER (middle)  | HV ZONE (right) |
 |                            |                 |                 |
 |  J1 cable (5V,GND,SDA,SCL) |  U2 + L1 + D1   |  U3 + Q1 + L2   |
 |  LDO 3.3 V                 |  (5 V -> 20 V)  |  D2, C_out,     |
 |  U1 MCP23008, A0-A2 jumpers|                 |  bleed, TVS     |
 |  coin inputs -> J3 (edge)  |                 |  K1 relay, Q3   |
 |  J5 spare, J4 voltage jmp  |                 |  J2 (to relay)  |
 +----------------------------------------------------------------+
   J1 and J3 on the left edge; J2 on the right edge, near the relay
```

### Layout rules

- **Net classes in KiCad:** an **`HV`** class (everything from C_out and D2 to
  K1, Q3, the divider and J2): **1.0 mm clearance** to everything else
  (IPC-2221 external bare copper needs only about 0.6 mm up to 150 V; use the
  extra), 0.4 mm tracks. Keep 0.8 mm for the 20 V and logic side.
  **Solder mask is not insulation.**
- **Keep the HV zone together** on the right and away from the cable header and
  the logic. **No logic traces under HV parts.**
- **Ground:** pour GND on the bottom layer, but **pull the pour back at least
  1.0 mm from HV nets** (or leave it out under the HV zone). Keep each stage's
  switch loop (SW, inductor, diode, output cap) **short and tight**; keep the
  FB divider and its trace **short and away from the inductors and switch
  node**; one star point where the stages meet GND.
- **Divider resistors** (3 x 200 k chain) in a straight line, evenly spaced;
  put the **bleed chain** next to C_out.
- **Relay K1:** contacts on the HV side, coil on the logic side, with **at
  least 2 mm or a slot** between them if the pins allow. **Q3 and R_sense**
  next to it; J2 on the same edge keeps the coil wires short.
- **Mounting holes** (M3) at the corners, not near HV. **Silkscreen** an HV
  warning, the J4 voltage table, and "capacitor stays charged".
- **Test points:** 20 V, 3.3 V, HV (a probe pad with a series resistor), GND.
- **Assembly:** C_out, K1, the Molex KK headers and likely L2 are through-hole
  or large, so expect to hand-solder them (mark them "do not place" for
  JLCPCB, as on the main board).
- **DRC:** set the net classes first, run DRC with the 1.0 mm rule, then check
  clearances by hand around C_out, D2 and J2.

## 13. All-through-hole version, with DigiKey parts (2026-10-27)

**Questions answered first**

- **Does a 100 kHz I2C bus affect the ES8388's audio?** No. Audio goes over I2S
  (a separate set of pins). I2C only writes the codec's registers (volume,
  mute, routing). The firmware already runs the codec at 100 kHz
  (`es8388.c`: `.scl_speed_hz = 100000`), so nothing changes.
- **Lots of little passives?** Yes: roughly **55-60 small parts**, all easy to
  get in through-hole form (see the count below). The catch is the two
  chips I picked, **MT3608/SX1308 (SOT-23-6) and TPS40210 (MSOP-10)**, and the
  200 V FET, which only come as surface-mount. A hand-soldered, all
  through-hole board needs **different chips**, below.

**Through-hole chip swap** (needs its own datasheet check: the earlier
equations and dividers were for the SMD chips)

| Job | Was | Through-hole replacement |
|---|---|---|
| Stage 1, 5 V to 20 V | MT3608 / SX1308 | **MC34063A** (DIP-8, 1.5 A switch, boost with a 1.25 V reference) |
| Stage 2, 20 V to 130 V | TPS40210 + SMD FET | **UC3843A** (DIP-8 current-mode controller) + **IRF630** (TO-220) |
| Gate level shifter and relay driver | 2N7002 | **2N7000** (TO-92) |
| I/O expander | MCP23008-E/SO | **MCP23008-E/P** (DIP-18) |

Design notes for the swap: the UC3843's UVLO needs about 8.4 V to start, so
it runs from the 20 V stage-1 rail (stage 1 has to start first; HV_EN gates
stage 1, and stage 2 shuts down by pulling its COMP pin low). Its gate drive
is close to its 20 V supply, so clamp the IRF630 gate with a 15 V zener.
Q3 (the firing FET) is also an IRF630: drive its gate from the +20 V rail
through a resistor and a 12 V zener, pulled down by two 2N7000s from GP6
(non-inverting), so it's off whenever HV_EN is off.

**DigiKey parts (looked up 2026-10-27; prices qty 1, stock as listed then)**

| Ref | Part | Maker / part number | DigiKey | Price | Notes |
|---|---|---|---|---|---|
| U1 | MCP23008 I/O expander, DIP-18 | Microchip **MCP23008-E/P** | MCP23008-E/P-ND | $1.54 (qty 100 $1.24) | 4,996 in stock, active |
| U2 | stage 1 controller, DIP-8 | onsemi **MC34063AP1G** | MC34063AP1GOS-ND | $1.98 | 6,945 in stock. Cheaper: KA34063A, $0.40 |
| U3 | stage 2 controller, DIP-8 | onsemi **UC3843AN** ([link](https://www.digikey.com/en/products/detail/onsemi/UC3843AN/610126)) | (see link) | $0.34 | the listing's description reads "buck", so **verify** it is the UC3843A you expect; TI's UC3843AN is $1.79 |
| Q1, Q3 | 200 V N-FET, TO-220 | Vishay **IRF630PBF** ([link](https://www.digikey.com/en/products/detail/vishay-siliconix/IRF630PBF/812782)) | (see link) | $2.43 each | 200 V, 9 A from memory: **verify**. Needs a heatsink only if it ever runs continuous |
| Q2, Q4, Q5 | small N-FET, TO-92 | **2N7000** (e.g. [STMicro](https://www.digikey.com/en/products/detail/stmicroelectronics/2N7000/654416)) | (see link) | $0.06-$0.66 | relay driver and the level shifter |
| K1 | DPDT relay, 5 V coil, through-hole | Omron (Aratas) **G6S-2-DC5** ([link](https://www.digikey.com/en/products/detail/aratas-formerly-omron-components/G6S-2-DC5/333982)) | (see link) | $4.52 | **250 VAC / 220 VDC switching** per the listing; 11,369 in stock |
| D1 | stage-1 Schottky | **1N5819** (DO-41, 40 V, 1 A), e.g. [TSC](https://www.digikey.com/en/products/detail/taiwan-semiconductor-corporation/1N5819/7357079) | (see link) | $0.32 | 1 A average is plenty here |
| D2 | 200 V ultrafast | **UF4003** (DO-41, 200 V, 1 A), e.g. [onsemi](https://www.digikey.com/en/products/detail/onsemi/UF4003/1048948) | (see link) | $0.64 | cheaper makers $0.02-$0.20: check reverse recovery on the datasheet |
| D3 | 150 V clamp | **1.5KE150A** (DO-201), e.g. [TSC](https://www.digikey.com/en/products/detail/taiwan-semiconductor-corporation/1-5KE150A/15783456) | (see link) | $0.19 | **standoff is 128 V**, not 150 V: fine only with the 128 V profile as the highest setting. 1.5KE170A (145 V) if you want more headroom, with a higher clamp |
| C_out | 220 uF / 200 V radial | Nichicon **UCS2D221MHD** ([link](https://www.digikey.com/en/products/detail/nichicon/UCS2D221MHD/4991877)) | (see link) | $3.08 | 18 mm dia x 26.5 mm, 7.5 mm lead spacing, 15,508 in stock. **Bigger than the 16 x 25 mm I assumed** |
| L2 | 220 uH, 1 A | Bourns **RLB9012-221KL** ([link](https://www.digikey.com/en/products/detail/bourns-inc/RLB9012-221KL/1969608)) | (see link) | $0.66 | 650 mohm DCR, Isat not listed: **check Isat against the controller's peak current**; Wurth 7447471221 ($2.49, 1.3 A) is the sturdier choice |
| L1 | 47-100 uH, 2 A+ | not found | | | **search DigiKey** for a through-hole 47-100 uH inductor with Isat >= 2 A (my search returned nothing) |
| J2 | 3-pin KK 396 header | Molex **0026604030** | WM4621-ND | $0.35 (10: $0.30) | 26,865 in stock |
| J3 | 4-pin KK 396 header | Molex **0026604040** | WM4622-ND | $0.44 (10: $0.38) | 57,598 in stock |
| J1 | 5-pin header, 2.54 mm | generic 1 x 5 | | | mates with the v6 J6 |

The listed parts come to about **$19.50** at qty 1, with a pair of
IRF630s ($4.86) the biggest single item after the relay ($4.52), the
capacitor ($3.08) and the DIP chips. Add roughly **$5-$8** for the passives
and the unlisted parts (L1, header, diodes, zeners, trimmer): call it **$25-$28
a board** at DigiKey qty-1 prices, before the bare PCB. Prices from page listings;
quantity breaks will drop it.

**Small parts count (all through-hole, 1/4 W metal film and radial or
ceramic disc)**, rough, to finalize with the schematic: about **25
resistors** (divider chain, pulldowns, pull-ups, gate and base resistors,
current sense), **12 capacitors** (decoupling, timing, bulk), **6-8 diodes and
zeners**, plus A0-A2 and J4 jumpers. Not cheap in hand-soldering time (about an
hour for a few boards) but cheap in parts.

**Do these before ordering**
1. Check each datasheet for the stage-1 and stage-2 equations: MC34063A boost
   at 5 V in and the UC3843A boost at 20 V to 130 V. The values in sections 5-6
   were for the SMD chips.
2. Check the IRF630's gate drive, Isat on L2, and the relay's contact rating at
   the datasheet, not the listing.

## Sources

- [TI TPS40210 datasheet SLUS772G](https://www.ti.com/document-viewer/lit/html/SLUS772G/GUID-1F0C084C-9CDC-454B-B44B-7A8ED312445A)
- [Datasheet listing: TPS40210](https://www.rlocman.es/datasheet/data.html?di=688933)
- [Omron G6S-2 5 VDC (Conrad)](https://www.conrad.com/en/p/omron-g6s-2-5-vdc-pcb-relay-5-v-dc-2-a-2-change-overs-1-pc-s-503840.html)
- [Panasonic TX2-5V (Arrow)](https://www.arrow.com/en/products/tx2-5v/panasonic?region=nac)
- [Panasonic TQ2-5V (Cytech)](https://www.cytechsystems.com/product/tq2-5v)
- [MCP23008 datasheet](https://ece-classes.usc.edu/ee459/library/datasheets/MCP23008.pdf)
- [MT3608 guide (LCSC)](https://www.lcsc.com/blog/mt3608-boost-converter-guide/)
- [ES2D (DigiKey)](https://www.digikey.com/en/products/detail/smc-diode-solutions/ES2D/6022516)
- [FQT4N20L (findchips): obsolete](https://findchips.com/detail/FQT4N20LTF)
- [STN4NF20L (ST)](https://www.st.com/en/power-transistors/stn4nf20l.html)
- [STN4NF20L (DigiKey)](https://www.digikey.com/en/products/detail/stmicroelectronics/STN4NF20L/2440279)
- [Molex 0026604040 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604040/197447)
- [Molex 0026604030 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604030/96502)
- [MUR220 (onsemi)](https://www.onsemi.com/products/discrete-power-modules/rectifiers/mur220)
- [SX1308 pinout (mikrocontroller.net)](https://www.mikrocontroller.net/part/SX1308)
