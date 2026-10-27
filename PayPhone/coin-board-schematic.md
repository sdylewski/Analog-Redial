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
 GND  --- J1.3   |        +-> sensor +5V (J3, JP1)                    |                              |
 SDA  --- J1.4 --+--> [U1 MCP23008 @0x20] GP4 HV_EN ----------------+                              |
 SCL  --- J1.5      |        GP5 POL --> Q2 --> K1 (DPDT relay) <------ HV+ ------------------------+
 INT  --- J1.6(opt) |        GP6 FIRE --> Q3 (200 V FET, low side) --> R_sense --> GND
                    |        GP0..2 <-- coin inputs <-- J3 (coin sensor)
                    |        GP3, GP7: spare, to J5

 K1 poles --> J2 (coil: A / B).  Relay on the coil: HD550002C, white + green wires.
```

## 2. Connectors

| Ref | Pins | Function |
|---|---|---|
| **J1** | 6 x 2.54 mm | 1 +5 V, 2 +3.3 V, GND, SDA, SCL, INT (INT via 0 ohm jumper, **DNP by default**: Analog Redial has no spare GPIO) |
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
- **OVP: a 150 V TVS or zener clamp across C_out** (SMBJ150A class, standoff
  150 V, breakdown about 167 V **[verify]**) so a feedback fault clamps below
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
