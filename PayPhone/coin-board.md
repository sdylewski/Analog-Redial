# Payphone coin board: design, wiring and parts

**Released for fab, untested.** The PCB layout is finished (2026-11-08), but nothing here has
been built, tested or simulated yet. Items marked **[verify]** are to be checked on the first
board. Reference designators and pin numbers match
`Payphone_coin_board_v1/Payphone_coin_board_v1.kicad_sch`. For the short public version, see
[docs/hardware-coin-board.md](../docs/hardware-coin-board.md); for the phone and its firmware,
[README.md](README.md) and [payphone-firmware.md](payphone-firmware.md).

| Part | Covers |
|---|---|
| [1. Design](#part-1-design) | how the board works: connectors, logic, the two power stages, coin inputs, cable, bench checks |
| [2. Output stage wiring](#part-2-coin-relay-output-stage-what-connects-where) | relay driver, polarity relay and firing switch, pin by pin |
| [3. Parts and PCB layout](#part-3-parts-and-pcb-layout) | parts list, what JLCPCB places, layout and routing |

The bench measurements of the coin relay and the patent notes are in
`Scott/coin-return-board.md` (local only, not in git).


## Part 1: Design

One board, surface-mount with a few through-hole parts, that (1) fires the
HD550002C coin relay in either direction from a high voltage made on the
board, (2) reads the phone's 5 / 10 / 25 cent coin switches, and (3) talks to
Analog Redial over I2C. Options for other payphones: a relay with two coils
and a common wire (J10), 12-24 V coils (J14 off, section 6), a fourth coin
input, inputs that take a coin acceptor's logic output up to 40 V (section 8),
screw terminals beside the phone's own plugs, and a spare analog input (J13). It needs only **5 V, VIO, GND, SDA, SCL** from the
main board. VIO is the host's I2C logic voltage (3.3 V from Analog Redial,
3.0 to 5.5 V works; section 3).

### 1. Block diagram

```
 Analog Redial                         COIN BOARD
 (v6 J11)                                                      HV zone (up to 119 V)
 +5V  --- J6.5 ---------> [U2 MC34063 5V->20V] --> 20V --> [U3 UC3843 + Q6 + L2 + D2] --> HV (C1)
 VIO  --- J6.4                  ^ Q1                                                        |
 SDA  --- J6.3 --+              |                                                           |
 SCL  --- J6.2 --+--> [U1 TCA9534A @0x39] P6 EN_HV                                          |
 GND  --- J6.1   |                        P5 POL  --> Q2 --> K1 (DPDT relay) <--------------+
                 |                        P4 FIRE --> Q4, Q5 --> Q3 (low side) --> R26 --> GND
                 |                        P0-P3 <-- D16-D19 <-- coin inputs <-- J3 / J12
                 +--> [U4 ADS1115 @0x48]  HV, 20V, firing current; AIN3 spare (J13)

 K1 poles --> J2 / J11 --> coin relay HD550002C (white, green wires).
 J10 shunt: J11 pin 2 to HV, for a relay with two coils and a common (J2 pin 2 isn't connected).
 J14 shunt: 20V to U3's VCC. Without it stage 2 can't run and HV stays at about 19 V.
```

### 2. Connectors

The phone has **two separate cables** for this board: a 4-wire one from the
coin switches and a 3-wire one from the coin relay. Both have a red and a
green wire, and they aren't the same wires.

| Ref | Part | Pins |
|---|---|---|
| **J6** | 5 x 2.54 mm pin header | **1 GND, 2 SCL, 3 SDA, 4 VIO, 5 +5 V**: the same pin numbers as the main board's v6 J11 (pin 4 there is +3.3 V), so a straight 1:1 cable, pin 1 to pin 1 |
| **J1** | JST XH 5-pin (B5B-XH-A, mates with XHP-5) | same five signals, same order, in parallel with J6: fit whichever suits the cable |
| **J2** | Molex KK 396, 3-pin, 0026604030 | **coin relay cable.** 1 green = COIL_A, 2 red = **not connected** (the HD550002C's own switch), 3 white = COIL_B (checked on the phone's plug; turn on Swap, section 7). KK 396 is rated 250 V |
| **J11** | Phoenix MKDS 1,5/3-5,08 screw terminal | 1 COIL_A, 2 COIL_C, 3 COIL_B: for a relay without the KK 396 plug, and the only connector for a two-coil relay's common (pin 2). Fit J2 or J11 |
| **J10** | 1 x 2 pin header + shunt | **two-coil relays only:** ties COIL_C (J11 pin 2) to HV (section 7). It can't reach J2, so a relay on J2 is unaffected either way |
| **J14** | 1 x 2 pin header + shunt | **HV boost:** 20V to U3's VCC. **No shunt (the safe default): HV stays at about 19 V**, whatever the firmware does. Shunt fitted: HV goes to J7-J9's setting (section 6) |
| **J3** | Molex KK 396, 4-pin, 0026604040 | **coin switch cable.** 4 green = common (GND), 3 white = quarter, 2 red = dime, 1 blue = nickel (checked on the phone's plug). On the plug with its tab at the bottom, green is the left pin and blue the right |
| **J12** | KF128 screw terminal, 5 pins, 2.54 mm pitch (coin inputs are low voltage) | 1 GND, **2 coin 4**, 3 quarter, 4 dime, 5 nickel (left to right on the board: nickel, dime, quarter, coin 4, GND; nickel, dime, quarter and GND are J3's pins, in parallel). Coin 4 goes to U1 P0 |
| **J13** | 1 x 3 pin header, 2.54 mm | spare analog input: 1 VIO, 2 AIN3 (through R52 10 k, C25 100 nF to GND), 3 GND (section 9) |
| **J7-J9** | three 2-pin headers + shunts | HV setting with J14 fitted: no shunt = 52 V; any combination is valid, up to 119 V with all three (section 6) |
| **JP1** | solder jumper, **open** | **VIO = 5 V**: bridge it only for a host with no logic supply to give (a 5 V I2C host using four wires). Leave it open whenever pin 2 is wired |

**Checked:** both plugs match the pin numbers above (green on J3 pin 4, white on J2 pin 3).

### 3. I2C and VIO

| Device | Address | Notes |
|---|---|---|
| ES8388 (main board) | 0x10 | already there |
| TCA9534A (main board v6) | 0x38 | line 2's expander, A0-A2 to GND |
| **U1 TCA9534APWR** | **0x39** | A0 to VIO, A1 and A2 to GND |
| **U4 ADS1115** | 0x48 | ADDR to GND |

The TCA9534A's address is 0x38 plus its A2 A1 A0 pins read as a binary number,
so another add-on can use 0x3A-0x3F. The main board has the I2C pull-ups; this
board's R42 and R43 (4.7 k, on SCL_IN and SDA_IN) are not fitted, and are there for testing
the board alone or using it with another host.

**Cable entry.** J1 and J6 carry +5V_IN, SCL_IN and SDA_IN. F1, a 1.1 A PTC, feeds the board's
+5V, with C20 (100 uF) after it to damp the cable. R40 and R41 (100 ohm) lead from SCL_IN and
SDA_IN to U1 and U4's SCL and SDA. F1 is there because +5V reaches the
20 V rail through L1 and D1 even with stage 1 off.

**VIO: the logic supply comes from the host.** U1 and U4 run from J6 pin 4,
not from a regulator on this board. A chip doesn't read an I2C high below 0.7 x
its supply, so at 5 V these chips wouldn't see a 3.3 V host's highs, and they
have to run at the host's own logic level. Taking it from the host does
that for a 3.3 V host (Analog Redial, ESP32, Pi) and a 5 V one (Arduino Uno)
alike, with no jumper to set. The 5 V pin powers only the converter and K1. A
host that has only 5 V bridges **JP1**.

Unpowered cases are safe: 5 V without VIO leaves the pulldowns holding EN_HV,
POL and FIRE low; VIO without 5 V has nothing to drive.

**VIO must be 3.0-5.5 V.** The chips work lower (U1 from 1.65 V, U4 from
2.0 V), but EN_HV and FIRE drive 2N7002 gates (Q1, Q4; threshold up to 2.5 V)
and the debug LEDs need about 3 V. Below that the HV supply simply won't turn
on.

### 4. U1 pins (TCA9534APWR, TSSOP-16, VCC = VIO)

| Pin | Name | Net | Dir | Idle | Notes |
|---|---|---|---|---|---|
| 1 | A0 | VIO | | | address 0x39 |
| 2, 3 | A1, A2 | GND | | | |
| 4 | P0 | Coin 4 | in | high | wired like P1-P3 (R49 10 k pull-up, R50 1 k, C24 100 nF), from J12 pin 2. Next to P1-P3, so the four coin inputs are together |
| 5 | P1 | Quarter | in | high | R4 10 k pull-up to VIO, R7 1 k series, C2 100 nF on the switch side |
| 6 | P2 | Dime | in | high | the same with R5, R8, C3 |
| 7 | P3 | Nickel | in | high | the same with R6, R9, C4 |
| 8 | GND | GND | | | |
| 9 | P4 | **FIRE** | out | low (R1 10 k) | drives Q3: current through the coil |
| 10 | P5 | **POL** | out | low (R2 100 k) | drives Q2 and K1: sets which way the coil is driven |
| 11 | P6 | **EN_HV** | out | low (R3 100 k) | turns on stage 1, and so stage 2: HV exists only while high |
| 12 | P7 | spare | | | not connected. The firmware makes it an output, low, so it doesn't float |
| 13 | ~INT | | | | not connected |
| 14 | SCL | SCL | | | |
| 15 | SDA | SDA | | | |
| 16 | VCC | VIO | | | C16 100 nF |

At power-up the TCA9534A is all inputs (high impedance, no internal pull-ups),
so the pulldowns hold EN_HV, POL and FIRE low: **nothing fires and the HV supply
is off** until firmware says so. **Firmware order matters:** the output register
powers up as all ones, so write 0 to it (0x01) *before* making P4-P6 outputs in
the configuration register (0x03); the other order drives them high for a
moment.

**Debug LEDs** (green 0805), each with a 1.5 k resistor, dim on purpose:

| LEDs | Wired | Lit while |
|---|---|---|
| D8 / D9 / D10 / D15 (R34-R36, R51) | VIO -> 1.5 k -> LED -> NICKEL_SW / DIME_SW / QUARTER_SW / COIN4_SW (the board side of D16-D19) | that coin input is low |
| D11 / D12 / D13 (R37-R39) | FIRE / POL / EN_HV -> 1.5 k -> LED -> GND | that output is high |

The coin LEDs take their current from VIO through the coin switch, not through
R4-R6, so they don't change what U1 reads. Current: about 0.3-0.5 mA at
VIO = 3.3 V (the LED's forward voltage is 2.6-3.1 V), about 1.5 mA at 5 V. A coin
pulse is short, so D8-D10 only blink. The HV indicator (D7, red) is in
Part 2.

### 5. Stage 1: 5 V to 20 V (U2, MC34063ADR2G)

1.25 V reference, internal 1.5 A switch, boost topology per the datasheet
application circuit. The switch collector's absolute maximum is **40 V**, so
the 20 V output (switch node about 21 V with the diode drop) is safe.

Datasheet designators (TI SLLS636N's step-up converter figure) on this board:

| Datasheet | This board | Value |
|---|---|---|
| VIN | +5V | 5 V |
| RSC | R11, R45, R46 | **3 x 1 ohm in parallel = 0.33 ohm** |
| pin 8 to pin 7 resistor | R12 | **100 ohm, 1206**: about 40 mA of drive at 5 V, a forced beta of 25 at the 1 A limit. 0.13 W |
| L | L1 | **100 uH**, SXN SMDRI127-101MT |
| 1N5819 | D1 | SS34 (SMA, 3 A 40 V) |
| CO | C7 | 100 uF / 50 V |
| CT | C8 | **1 nF** |
| R1 (bottom) | R14 | **10 k, 1 %** |
| R2 (top) | R13 | **150 k, 1 %** |
| input cap | C5, C6 | 100 uF + 100 nF |

| Pin | Name | Net / connection |
|---|---|---|
| 1 | SwC (switch collector) | The switch node: L1 (far end) and D1 anode. Switch on: current flows +5V -> R11/R45/R46 -> L1 -> pin 1 -> pin 2 -> GND. Off: the inductor kick forward-biases D1 and charges C7 |
| 2 | SwE (switch emitter) | **Straight to GND** |
| 3 | TC (timing cap) | C8 to GND. Nothing else on this pin |
| 4 | GND | GND |
| 5 | Vfb | Divider midpoint: R13 from 20V, R14 to the **Q1 drain** (Q1 source = GND, gate = EN_HV) |
| 6 | Vcc | +5V (the input side of R11/R45/R46), with C5/C6 to GND |
| 7 | Ipk (peak current sense) | The L1 side of R11/R45/R46. The cycle ends when the drop across them reaches about 330 mV |
| 8 | DC (driver collector) | Through R12 to pin 7 |

**Feedback.** Vout = 1.25 x (1 + R13/R14): 150 k and 10 k give 1.25 x 16 =
**20.0 V**.

**Operating point** (5 V in, 20 V out, up to 100 mA, about 33 kHz): about 24 us on and
6 us off, 1 A switch peaks, about 0.2 V p-p ripple on C7. The input draws about 0.4 A
average with about 1 A peaks. A hard-saturated MC34063 switch can be slow to turn off at
low currents above 30 kHz; each pulse here peaks near 1 A, so it should be fine
**[verify the switch-node waveform]**.

**Enable.** The MC34063 has no enable pin, so **Q1 (2N7002) sits in the bottom
leg of the feedback divider**:

```
  20V ---[ R13 150k ]---+--- U2 pin 5 (Vfb)
                        |
                   [ R14 10k ]
                        |
                     Q1 drain
  EN_HV --- Q1 gate     (R3 100k pulldown on EN_HV)
                     Q1 source --- GND
```

- **EN_HV high:** Q1 on (a few ohms, negligible against 10 k), the divider
  works, the rail is 20 V.
- **EN_HV low:** R14 is open, R13 pulls pin 5 up to the rail, above the 1.25 V
  threshold, so the comparator inhibits the switch, the same mechanism as normal
  regulation. An open gate, a broken wire or a reset all leave it off.
- **The rail doesn't go to 0 V when disabled.** A boost converter always has a
  DC path +5V -> L1 -> D1 -> output, so 20V idles at about 4.6 V. That is below
  U3's 8.4 V start threshold, so **stage 2 shuts off by itself**.
- At VIO = 3.3 V a high-threshold Q1 may be tens of ohms, which shifts the rail by
  about 1-3 %.

With EN_HV low the board draws a few mA and holds no high voltage. Stage 1 starts in
tens of ms and stage 2 reaches 52-60 V in well under 1 s (119 V in about 1 s).

### 6. Stage 2: 20 V to 52-119 V (U3, UC3843BD1R2G + Q6, STN4NF20L)

Current-mode PWM controller, non-synchronous boost, driving an external N-FET.
It starts at about 8.4 V (7.8 to 9 V) and stops at 7.6 V, so it runs from the
20 V rail and stage 1 must start first. The UCx843 runs to about 97 % duty; the
UCx844/5 are limited to 50 % and wouldn't work.

| Ref | Function | Value | Notes |
|---|---|---|---|
| **Power stage** | | | |
| L2 | Boost inductor (20V to SW2) | **220 uH**, SXN SMDRI127-221MT | shielded, 2.0 A Isat, 0.39 ohm |
| Q6 | Switch (drain SW2, source to Rcs1) | **STN4NF20L** (SOT-223, 200 V, 1 A, 1.35 ohm; C7603336, the same part as Q3) | 0.45 A peak, about 0.15 A RMS: 30 mW on, and 3.2 nC of gate charge switches it in about 10 ns, so switching loss is a few mW. Turn-on is at zero current (DCM) |
| D2 | Output diode (SW2 to HV) | **ES2D** (SMA, 200 V, 2 A ultrafast) | cathode toward C1. Average current is only tens of mA |
| C1 | HV output capacitor | **220 uF / 200 V** | Nichicon UCS2D221MHD, 18 x 26.5 mm, 7.5 mm leads |
| C14 | Input decoupling on 20V | **100 uF / 50 V** | at L2 and Q6's source return: it carries stage 2's switching current, so it can't share C7 at stage 1 |
| **Gate drive** | | | |
| R54 | U3 OUT to Q6 gate | **100 ohm** | damps ringing and limits the current into Dz1: OUT swings to about 18.5 V, so Dz1 sees about 35 mA each on-time; switches Q6 in about 10 ns |
| Dz1 | 15 V gate clamp (cathode to gate, anode to Q6 source) | **1SMA4744A** (1 W) | OUT is just under the STN4NF20L's 20 V Vgs maximum, so this is margin |
| R53 | Q6 gate to source | **10 k** (C17414, Basic) | holds Q6 off while U3 has no supply (J14 open, or stage 1 off), when nothing else drives the gate |
| **Current sense** | | | |
| Rcs1 | Q6 source to GND | **2.2 ohm**, 0805 (C17521, Basic) | Ipk = 1 V / Rcs1 = 0.45 A. About 0.15 A RMS, so 0.05 W, inside the 0805's 0.125 W |
| R56 | Rcs1 to CS pin 3 | **1 k** | with Cf1, filters the leading-edge spike (the UC3843 has no blanking) |
| Cf1 | CS pin 3 to GND | **470 pF**, X7R | 0.47 us, well under the 15 us period |
| Rslope1 | RT/CT ramp to CS, in series with Cslope1 | **27 k, fit only if needed** | The converter runs in discontinuous mode (below), where subharmonic oscillation can't happen. Fit it if the switch node shows alternating long and short pulses |
| Cslope1 | DC block in the same path | **1 nF** | passes only the ramp |
| **Oscillator** | | | |
| R18 (Rt) | VREF pin 8 to RT/CT pin 4 | **12 k** | fOSC = 1.72 / (Rt x Ct) = 65 kHz |
| Ct1 | RT/CT pin 4 to GND | **2.2 nF, C0G** | sets the clock, so no X7R. Never below about 1000 pF |
| **Feedback** | | | |
| Rfbt1-3 | HV to FB pin 2 | **3 x 100 k in series** (1206) | about 40 V each |
| Rfbb1, Rfbb2, R55, Rfbb4 | FB pin 2 to GND | **Rfbb4 15 k always fitted**; R55 100 k, Rfbb2 30 k, Rfbb1 22 k added in parallel by J9 / J8 / J7 | table below |
| Rz1 | COMP pin 1 to FB pin 2, in series with C28 | **1 M** | mid-band gain Rz / Rfbt = 3.3 |
| C28 | In series with Rz1 | **100 nF**, X7R, 0805 100 V (C28233, Basic, the same part as C17) | zero at 1.6 Hz |
| Cp1 | COMP pin 1 to FB pin 2, across Rz1 + C28 | **100 pF, C0G** | pole at 1.6 kHz, keeps switching noise out of the error amplifier |
| **Decoupling** | | | |
| C26 | VCC pin 7 to GND | **0.1 uF, 50 V** | on the 20 V rail, so not a 16 or 25 V part |
| C27 | VREF pin 8 to GND | **0.1 uF** | required by the datasheet |
| **Protection** | | | |
| D3 | Clamp across C1 | **SMBJ130A** (SMB) | 130 V standoff; cathode to HV |
| R57-R59 | Bleed across C1 | **3 x 1 M in series** (1206) | section below. 4.7 mW at 119 V, about 40 V each |
| C21-C23 | Bypass across C1 | **3 x 100 nF 50 V in series** (1206, X7R), middle nodes on HV_B1 and HV_B2 | C1 is slow at high frequency; about 33 nF, less at 119 V. R57-R59 share the voltage, about 40 V each at 119 V |

| Pin | Name | Net / connection |
|---|---|---|
| 1 | COMP | E/A output. Rz1 + C28 to pin 2 |
| 2 | FB | Divider midpoint. Internal reference is **2.5 V**, so HV = 2.5 x (1 + Rfbt / Rfbb) |
| 3 | CS | R56 from the top of Rcs1; Cf1 to GND. The cycle ends at **1 V** |
| 4 | RT/CT | R18 from pin 8, Ct1 to GND |
| 5 | GND | GND |
| 6 | OUT | Q6 gate through R54 |
| 7 | VCC | 20V **through J14** (net U3_VCC), C26 to GND on U3's side. 30 V absolute maximum, about 11 to 17 mA running |
| 8 | VREF | **5 V** reference; it only feeds R18 here. C27 to GND |

**Voltage select.** With J14 fitted, each J7-J9 shunt adds a resistor in parallel with Rfbb4,
which lowers Rfbb and raises HV. **Every combination is valid** (the same table is on the
silkscreen and the schematic):

| Shunts fitted | Rfbb total | HV (about) |
|---|---|---|
| no J14 | | **19 V**, stage 2 off (the default) |
| J14 alone | 15 k | **52 V** |
| J14 + J9 | 13.0 k | 60 V |
| J14 + J8 | 10 k | 78 V |
| J14 + J9 + J8 | 9.1 k | 85 V |
| J14 + J7 | 8.9 k | 87 V |
| J14 + J9 + J7 | 8.2 k | 94 V |
| J14 + J8 + J7 | 6.9 k | 112 V |
| J14 + J9 + J8 + J7 | 6.4 k | **119 V**, the maximum |

**J14, the HV boost jumper.** U3 gets its supply only through J14. **With no shunt (the
default), stage 2 can't run**, and a boost converter that isn't switching passes its input
straight through: HV is the 20 V rail less D2's drop, **about 19 V**, from stage 1 alone. No
firmware setting can raise it. That's the setting for 12-24 V coils. Fit the J14 shunt for
the HD550002C and other high-voltage relays; J7-J9 then set the voltage. R53 holds Q6's
gate low while U3 is unpowered. At 19 V the current is set by the coil, Q3 and R26 (10 ohm):
a 50 ohm 24 V solenoid draws about 0.3 A, so R26 takes about 0.9 W for the pulse; keep pulses
short **[verify]**. D7 stays dark at 19 V (it lights above about 24 V).

No shunt mistake can go above 119 V, below D3's 130 V standoff, and without J14 nothing goes above about 19 V. A missing, lifted
or dirty shunt gives a lower voltage, never off or higher. An open Rfbb4 turns the
converter off (FB then sits at HV). An open Rfbt would let the output run away:
that's the failure D3 is there for.

**Operating point** (calculated; **[verify]** on the bench):

- **Frequency:** 65 kHz (1.72 / (12 k x 2.2 nF)).
- **Power:** about 1.7 W at 119 V, 2.2 W at 60 V, 2.4 W at 52 V from the 20 V rail. Stage 1
  supplies about 2 W, so while C1 charges below about 70 V, stage 2 can ask for more than
  stage 1 gives (bench check 2, section 11).
- **Charge time:** C1 holds 0.3 J at 52 V, 0.4 J at 60 V, 1.6 J at 119 V: a fraction of a
  second up to about 1 s.
- **Mode:** discontinuous once HV is above about 30 V (about 32 % duty), so no
  subharmonic oscillation and no right-half-plane zero.
- **Loop:** Rz1 + C28 (Type II) give a crossover of about 4-40 Hz across the voltage and load
  range, with 70-90 degrees of phase margin. A slow loop suits a large C1 and a one-pulse load.
- **C1 droop:** a 100 mA, 50 ms pulse drops it about 23 V.

**D3 and the bleed path, across C1:**

```
  HV ----+-------+-------+-----------+-----------+-----------+
         |       |       |           |           |           |
        C1      D3     Rfbt1-3     R57-R59       R32        R27, R28
      220uF   SMBJ130A  3x 100k     3x 1M       1M         D5, D7
      /200V              |           |           |         (HV LED)
         |       |       +-- FB      |         +-- AIN1      |
         |       |       |           |         R33           |
         |       |     Rfbb4         |          27k          |
         |       |    (+ J7-J9)      |           |           |
  GND ---+-------+-------+-----------+-----------+-----------+
```

- **D3** clamps HV at about 145-160 V if the feedback loop fails **[verify]**,
  below the 200 V parts' limit, at the cost of a clamp that may overheat if the
  fault persists.
- **Bleed.** The resistors total 228 k, a 50 s time constant with 220 uF, and the
  HV LED string adds 0.4-1.4 mA above 24 V. After EN_HV drops, 119 V falls below
  50 V in about 13 s, below 24 V (D7 goes out) in about 33 s and below 20 V in about
  42 s; 60 V falls below 20 V in about 32 s.
- **If a path fails.** Without the LED string it's about 89 s to 20 V. The divider
  only drains C1 while Rfbb4 is connected; if that opens, R32, R57-R59 and the LED
  string still reach 20 V in about 88 s. (A failed-open Rfbt runs the output up to
  D3, so that case is covered by the clamp, not the bleed.)

### 7. Firing and polarity

See Part 2 for the wiring.

**Sequence** (firmware): set POL, raise EN_HV, wait until HV is up (U4 reads it,
or a fixed delay of about 3 s), pulse FIRE, drop FIRE, drop EN_HV. **Never change
POL while FIRE is high or while current flows**, so K1's contacts only switch
dead. Cap FIRE at about 1 s. The relay's own switch (red-green) cuts the current
by itself, so FIRE doesn't need exact timing. If the current doesn't stop, firmware
should drop FIRE as soon as U4 shows it: the red-green contact can short the coil
(0.1 ohm), and Q3 then limits the current to about 1 A with R26, which it can't
stand for long at high HV.

**Two-coil relays (J10, J11).** Some coin relays have separate collect and return coils on
a common wire **[verify which phones]**. Wire them to J11 with the common on pin 2 and fit J10:
pin 2 is then HV, and K1 picks the coil. FIRE, POL and D14 work as before.

| POL | K1 | COIL_A (J2/J11 pin 1, green) | COIL_B (J2/J11 pin 3, white) | COIL_C (J11 pin 2) |
|---|---|---|---|---|
| low (at power-up) | off | HV | RETURN | J10 on: HV · J10 off: not connected |
| high | on | RETURN | HV | the same |

What a relay sees when FIRE pulses (HV = 60 V here):

| Relay | J10 | POL low | POL high |
|---|---|---|---|
| One coil, A-B (the HD550002C) | either (J2 pin 2 isn't connected) | +60 V on green: return | +60 V on white: collect (so turn on Swap) |
| Two coils, common on J11 pin 2 (coil 1 A-C, coil 2 C-B) | **on** | coil 2 fires (C at HV, B at RETURN); coil 1 has HV on both ends, 0 V | coil 1 fires (C at HV, A at RETURN); coil 2 has 0 V |
| Two coils, common on J11 pin 2 | off | both coils in series, about 30 V each: both pull weakly, don't use | the same, reversed |

In two-coil mode the common is always the positive end.

**J2 pin 2 isn't connected**, so the HD550002C's red wire (one side of its own switch) can
never reach HV, whatever J10 is set to. A relay whose common must be negative would need
COIL_C tied to RETURN instead of HV, which this board doesn't offer.

**D14** (ES2D, RETURN to HV) gives the coil's stored energy a path if Q3 turns off
while current flows. K1 always puts HV on one coil wire and RETURN on the other, so
one diode covers both directions. It can't clamp the relay's own contact opening,
which is in series with the coil.

### 8. Coin inputs

- **The coin switches are passive:** green is the common, and green to white / red
  / blue closes briefly for a quarter / dime / nickel. There's no supply to them:
  green goes to GND, and each line is pulled up to VIO.
- **Each line:** R4-R6 10 k pull-up to VIO, R7-R9 1 k series to U1, C2-C4 100 nF to
  GND on the switch side. D8-D10 show each line (section 4). **Coin 4** (U1 P0, J12 pin 2)
  is the same: R49, R50, C24, D15 with R51. It's for a 50c or $1 switch, a coin-present
  switch, or a coin acceptor's pulse output.
- **D16-D19 (B5819W Schottky, 40 V)** sit in series between each connector pin and its
  line, cathode toward the connector. A closure or an output pulling low still pulls the line
  low (about 0.3 V through the diode); a positive voltage on the pin, up to 40 V, just
  reverse-biases the diode, so U1 and the LEDs never see it. So the inputs take **a switch to
  GND, an open-collector output, or a push-pull logic output of any voltage up to 40 V**,
  all active low. A coin acceptor that signals active high needs its own inverter. They
  don't protect against a negative voltage.
- Each closure pulls its line low (active low). The contacts carry about 0.3 mA from
  the pull-up plus about 0.5 mA through the line's debug LED, enough wetting current
  for old contacts.
- Mechanical contacts bounce. The RC on each line (about 1 ms) takes out the fastest
  spikes; firmware polls U1 about every 2-5 ms, debounces for a few ms and counts
  edges.

### 9. Measuring HV, 20 V and firing current (U4, ADS1115)

U1 has no ADC, so U4 does the measuring (16-bit, 4 channels; its wiring table is in
Part 2). R44 (1 k) sits between R26 and AIN2,
because R26 can reach about 10 V in a coil-short fault, far above what AIN2 can take. Firmware can check "reached the
target within N s", and see **C1 sag after a fire**: if the relay doesn't fire the
voltage stays up; if it does, it sags by I x t / C (about 20 V for 100 mA x 50 ms).
U4 can be left off: firmware then falls back on a fixed delay and knows nothing about
whether the relay fired.

**AIN3 (J13)** is a spare input for whatever a payphone needs measured: a sensor, a
pickup on a three-slot phone's bells, a voltage. J13 gives VIO, the input and GND. R52 (10 k)
and C25 (100 nF to GND) filter it (a 1.6 ms time constant, fine at the ADS1115's rates of up to
860 samples a second) and limit a fault: 24 V on the pin puts about 2 mA into U4's protection
diodes, inside its 10 mA limit. Readings are 0 to VIO.

### 10. Cable from the main board

The board mounts in the phone, next to the relay, on a cable from the main board's
I2C header. The high voltage never leaves this board: the cable carries only 5 V,
VIO, GND, SDA, SCL. The only wires with high voltage are the short ones from J2 to
the relay.

- The limit is bus capacitance (400 pF max for I2C). Twisted pair is roughly 50-100 pF
  per metre, plus the board traces and devices, so **1 m is workable at 100 kHz** with
  the main board's 4.7 k pull-ups (rise time around 0.5 us, under the 1 us limit).
  **Don't run this bus at 400 kHz** with the long cable. The firmware already runs the
  bus at 100 kHz (`es8388.c`). Audio goes over I2S, so the bus speed doesn't touch it.
- **Twist each signal with a ground wire** (SDA+GND, SCL+GND), or use a shielded pair,
  so the cable doesn't pick up the boost converter's switching noise.
- **Keep the VIO wire** rather than making 3.3 V locally: the chips must run at the
  host's I2C level, which only the host knows (section 3). VIO draws a few mA, so cable
  drop doesn't matter.
- **Power on the cable:** charging C1 pulls about 1 A peaks through stage 1 for up to a
  second. Use **24 AWG or thicker** for 5 V and GND (or two wires each).
- **If the bus misbehaves** (the ES8388 loses its config when the cable is touched): add
  an I2C buffer (TCA4307 or PCA9517 class) at the main-board end
  so the cable can't load the codec's bus, or a differential extender such as the PCA9615
  for longer runs. **Probably not needed under 1 m.**

### 11. Bench checks for the first board

1. **Which voltage the phone needs:** the HD550002C works at about 60 V; J14 alone gives
   52 V and J14 + J9 gives 60 V. The higher settings are for other phones.
2. **Stage 1 against stage 2:** while charging, stage 2 can ask for more than stage 1's 2 W
   (section 6). If the 20 V rail sags to U3's 7.6 V stop, the converter hiccups: raise Rcs1
   for a lower peak current, or add a soft start on COMP.
3. **Overshoot at the end of a charge:** watch that HV stays below D3's standoff at 119 V.
   A smaller C28 (47 nF) cuts overshoot. Chirping from skipped cycles at light load is
   harmless.
4. **U2's switch-node waveform** (section 5), and whether charging pulls the 5 V rail below spec.
5. **Pull-in charge** (current x time at the operating voltage), to confirm C1 is large enough.
6. **J14 off:** U3 stays off (R53 holding Q6), and HV settles near 19 V.
7. **Coin inputs through D16-D19:** the low level U1 reads with a real switch (about
   0.3 V diode drop plus the 1 k), and a coin acceptor's output if one is to hand.
8. **Firmware** (`firmware/components/coinboard/`, tested only against simulated chips): the
   coin inputs' timing, U4's readings against a meter, the pulse length the relay needs, and
   the overcurrent limit. [payphone-firmware.md](payphone-firmware.md) has the
   bring-up steps.

### Sources

- [TCA9534A datasheet](https://www.ti.com/lit/ds/symlink/tca9534a.pdf)
- [Panasonic TX2-5V (DigiKey)](https://www.digikey.com/en/products/detail/panasonic-industry/TX2-5V/251803); the Omron G6S-2 DC5 fits the same footprint
- [ES2D (DigiKey)](https://www.digikey.com/en/products/detail/smc-diode-solutions/ES2D/6022516)
- [STN4NF20L (ST)](https://www.st.com/en/power-transistors/stn4nf20l.html)
- [STN4NF20L (DigiKey)](https://www.digikey.com/en/products/detail/stmicroelectronics/STN4NF20L/2440279)
- [Molex 0026604040 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604040/197447)
- [Molex 0026604030 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604030/96502)

## Part 2: Coin relay output stage, what connects where

Reference designators and pin numbers match
`Payphone_coin_board_v1/Payphone_coin_board_v1.kicad_sch`.

The output stage takes the high voltage on net **HV** (C1, 52-119 V, set by
J7-J9) and sends it through the payphone's coin relay one way or the other.
Three small blocks, each driven by one TCA9534A pin:

| Block | Driven by | Does |
|---|---|---|
| A. Relay driver | **POL** (U1 P5, pin 10) | energizes K1's 5 V coil |
| B. Polarity relay + coil connector | K1's contacts | picks which coin-relay wire gets HV |
| C. Firing switch | **FIRE** (U1 P4, pin 9) | lets current flow through the coin relay |

### The whole stage at a glance

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
   J2 pin 1 (green)          RETURN
   J2 pin 3 (white)             |
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

| POL (P5) | K1 | J2 pin 1, green | J2 pin 3, white | Coin relay does |
|---|---|---|---|---|
| low (default at power-up) | off | HV | RETURN | **return to caller** (lever right) |
| high | on | RETURN | HV | **collect** (lever left) |

The phone's plug has green on pin 1 and white on pin 3, so turn on **Swap** on the
Payphone tab: Collect then drives POL high. Confirm with a Collect test in the phone.

Nothing flows until FIRE goes high. **Change POL only while FIRE is low**, so
K1's contacts never switch a live current.

### Block A: relay driver (POL -> K1 coil)

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
| K1 TX2-5V | 1 (coil) | +5 V |
| K1 | 12 (coil) | Q2 pin 3 (C), D4 pin 2 (A) |
| D4 1N4148W | 1 (K, the band) | +5 V |
| D4 | 2 (A) | K1 pin 12 / Q2 collector |
| Q2 MMBT3904 | 3 (C) | K1 pin 12 |
| Q2 | 1 (B) | R20 to POL |
| Q2 | 2 (E) | GND |
| R20 1k | | Q2 base to POL: about 2.6 mA at VIO = 3.3 V, plenty for the coil's 30-40 mA |
| R2 100k | | POL to GND: keeps K1 off while U1 powers up |

### Block B: polarity relay and the coin-relay connector

K1 pins, from KiCad's G6S-2 symbol, the same on the TX2-5V (the arm of each switch rests on pins 3
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
| 1 | green | COIL_A |
| 2 | red (the relay switch's other side) | not connected |
| 3 | white | COIL_B |

**J11** (screw terminal) has COIL_A on pin 1 and COIL_B on pin 3, like J2, and **COIL_C on
pin 2**, which only J10 connects: its shunt ties COIL_C to HV, for a relay with two coils
and a common wire. J2 pin 2 isn't connected, so the HD550002C's red wire never sees HV.
Which coil fires for each POL setting: Part 1, section 7.

Checked on the phone's own plug: green on pin 1, white on pin 3. The coil doesn't care
which way round it is; only the Swap setting changes.

The coin relay's own switch (red-green) cuts the current once the lever has
moved, which is why the green wire is used: each fire is a single shot.

### Block C: firing switch (FIRE -> Q3)

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
     Q4 pin 1 (G) --- R21 1k --- FIRE --- U1 pin 9 (P4)
     Q4 pin 2 (S)             |     |
           |                  |     R1 10k
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
| R1 10k | | FIRE to GND: Q3 stays off while U1 powers up |
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
| low, or U1 powering up (R1 holds it low) | off | off | 0 V (R25) | no current |
| high | on | on | 12 V (D6) | current flows HV -> K1 -> coin relay -> K1 -> Q3 -> GND |

**FIRE with EN_HV low.** The 20V rail then sits at about 4.6 V (5 V coming through
L1 and D1) and U3 stays off. That's enough to turn on Q3 **[verify its threshold]**,
so FIRE high would still fire the relay from whatever C1 holds, until C1 has bled
down (D7 out). Firmware must keep FIRE low unless it means to fire.

### Net labels

The HV net class in `Payphone_coin_board_v1.kicad_pro` matches `/HV*`, `HV*`, `/COIL_A`,
`/COIL_B`, `/COIL_C`, `/RETURN` and `/SW2`, so keep these names: the 1.0 mm clearance then
applies automatically. (A logic net whose name starts with HV would pick up the HV class too,
so don't name one that way.)

| Label | Where |
|---|---|
| HV | C1+, C21 pin 1, D2, D3, D14 cathode, K1 pins 3 and 8, R27, R32, R57, Rfbt1 |
| HV_B1, HV_B2 | between R57-R59 (bleed) and between C21-C23 |
| HV_F1, HV_F2 | between Rfbt1-3 (feedback) |
| HV_LED1, HV_LED2 | R27, R28, D5 (HV indicator) |
| SW2 | Q6 drain, L2, D2 anode: swings up to HV |
| COIL_A | K1 pin 4, J2 pin 1, J11 pin 1 |
| COIL_B | K1 pin 9, J2 pin 3, J11 pin 3 |
| COIL_C | J11 pin 2, J10 pin 2 (J10 pin 1 is HV) |
| RETURN | K1 pins 5 and 10, Q3 pin 2, D14 anode |
| GATE3 | Q3 pin 1, D6 pin 1, R24, R25 |
| RLY_COIL, Q2_B, Q4_G, Q4_D, Q5_B, Q5_C | the small nodes in blocks A and C |
| ISENSE | Q3 pin 3, R26, R44 |
| ISENSE_ADC | R44, U4 AIN2 |

### HV safety

- **Bleed time.** The feedback divider (about 310 k), R32 + R33 (about 1 M), R57-R59
  (3 x 1 M) and the HV LED's string (below) drain C1: 119 V falls below 24 V (D7 out)
  in about 33 s and below 20 V in about 42 s. Without the LED string it's about 86 s
  to 20 V; if the divider opens, about 88 s. Still treat C1 as live until D7 is out,
  or short it through a resistor before touching.
- **HV indicator (D7).** HV -> R27 33 k -> R28 33 k -> D5 (BZT52C22, cathode to R28) ->
  D7 red 0805 LED -> GND. It lights above about 24 V (22 V zener + LED). With EN_HV low
  and C1 drained, HV idles at about 4 V, so D7 is dark; it lights whenever C1 is charged.
  It runs from C1 itself, so it stays lit while C1 drains, even with the board unpowered.
  Current: 0.4 mA at 52 V (dim but visible indoors), 1.4 mA at 119 V. R27/R28 share the
  voltage (about 67 mW and 47 V each at 119 V, inside an 0805's rating).
- **D3 orientation.** D3 (SMBJ130A) is unidirectional: pin 1, the cathode (band), is on HV
  and pin 2 on GND. Check the band when hand-soldering it.

### The ADS1115 (U4)

| U4 pin | Connects to |
|---|---|
| 8 VDD | **VIO**, with C17 100 nF (not +5 V: at 5 V the chip wants I2C highs above 3.5 V, more than a 3.3 V bus gives) |
| 3 GND | GND |
| 1 ADDR | GND: address 0x48 |
| 10 SCL, 9 SDA | SCL, SDA (R40 and R41 from the cable) |
| 2 ALERT/RDY | not connected |
| 4 AIN0 | 20V through R17 150 k / R19 27 k: 20 V reads 3.05 V (full scale 26.8 V). C19 100 nF to GND |
| 5 AIN1 | HV through R32 1 M / R33 27 k: 119 V reads 3.13 V. C18 100 nF to GND |
| 6 AIN2 | ISENSE (R26) through R44 1 k: 100 mA reads 1 V |
| 7 AIN3 | J13 pin 2 through R52 10 k, C25 100 nF to GND: spare analog input |

## Part 3: Parts and PCB layout

Reference designators and
values match `Payphone_coin_board_v1/Payphone_coin_board_v1.kicad_sch`.

The board is surface-mount, with through-hole only for C1, K1, U4's module and the
connectors. JLCPCB places every surface-mount part except seven that are hand-soldered:
**C5, C7, C14, C20, U3, F1 and D3**. Those have no LCSC number in the LCSC field (their
LCSC number is in the Note field), so they're left off the JLCPCB BOM and CPL.

### Parts

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

**U4 is optional.** It only adds readings (Part 1,
section 9). It's one of the common ADS1115 modules, not the bare chip; these carry their own
I2C pull-ups and decoupling, which is fine at VIO.

#### Resistors and small capacitors

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

#### Hand-solder parts from DigiKey

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

#### Ordering

Upload the files in `Payphone_coin_board_v1/jlcpcb/production_files/` (gerbers, BOM, CPL):

- 2 layers, **lead-free HASL**, top-side assembly, **Economic PCBA** (JLCPCB offers Economic
  only with HASL).
- Four parts carry JLCPCB's Extended-part fee (about $3 each per order): U1, Q3/Q6, L1 and L2.
- Estimated cost for 5 boards, delivered to the US with tariff and tax: about $130 from
  JLCPCB, about $70 for the DigiKey parts and $10 for five ADS1115 modules, so **about $40 a
  board**. An estimate, not a quote.

#### The JLCPCB plugin

`Payphone_coin_board_v1/jlcpcb/` is kicad-jlcpcb-tools' output (its `project.db` isn't
tracked). Before each export:

- Every surface-mount part JLCPCB places is in the CPL file, not just the BOM. A symbol marked
  "exclude from position files" carries that to its footprint on Update PCB.
- No hand-solder part has an LCSC number, and the plugin's **"Add parts without LCSC numbers to
  BOM/CPL" is off**, so hand-solder parts are left out of both files.

### Voltage ratings of small parts

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

### PCB layout

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

#### Net classes

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

#### Ground

One ground, no splits: there's no transformer, and FIRE, POL, I2C and the ADC lines all cross
between blocks. The bottom layer is one solid GND pour with only short jumpers. Each power loop
closes on the top layer and stitches to the pour with several vias at its capacitors.

#### Stage 1 and stage 2

- **Stage 1 hot loop:** U2 pin 1 -> D1 -> C7 -> GND -> U2 pin 2, all on top. R11/R45/R46 sit
  side by side with equal paths, so they share the current.
- **Stage 2 hot loop:** Q6 drain -> D2 -> C1 ... Rcs1 -> Q6 source. C21-C23 close its fast
  part, from D2's cathode to Rcs1's GND end.
- **SW2** (Q6 drain, L2, D2 anode) swings 0-120 V: short, with nothing running parallel to it.
- **Current sense:** a Kelvin trace from Rcs1's top pad to R56; Cf1 at U3's CS pin.
- **Quiet ground:** Ct1, C27, Cf1, Rfbb4 and J7-J9 return to U3 pin 5, then one via to the
  pour. The FB node is short, at U3 pin 2; HV runs out to Rfbt1, not the other way.
- **R32 sits at C1:** only its 3 V tap (AIN1) runs to U4, so no HV enters the logic block.

#### FIRE, POL and the ADC

- FIRE and POL leave U1 as VIO logic lines; the level shift is at Q3, so GATE3 is only a few
  millimetres long.
- **Firing loop:** C1+ -> K1 -> J2 -> coin relay -> K1 -> Q3 -> R26 -> GND -> C1-, compact,
  with R26's GND via toward C1-. K1's HV (3, 8) and RETURN (5, 10) pads cross over, so one pair
  runs on the bottom layer.
- Each ADC divider's low end and filter cap (C18, C19) sits at U4, so the long runs are the 3 V
  taps.
