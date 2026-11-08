# Payphone coin board: design

**Released for fab, untested.** The PCB layout is finished (2026-11-08), but nothing here has been
built, tested or simulated. Values marked **[verify]** come from
distributor listings or my memory of a datasheet, not from the manufacturer's
datasheet; check them before ordering. Reference designators match
`Payphone_coin_board_v1/Payphone_coin_board_v1.kicad_sch`.

| Doc | Covers |
|---|---|
| This one | how the board works: connectors, logic, the two power stages, coin inputs, cable, open items |
| [output-stage-wiring.md](output-stage-wiring.md) | relay driver, polarity relay and firing switch, pin by pin |
| [coin-board-smt.md](coin-board-smt.md) | parts list, what JLCPCB places, PCB layout and routing |
| [payphone-firmware-plan.md](payphone-firmware-plan.md) | the firmware: the Payphone tab (coin tones, coin required, messages, log) and the coin board's driver |
| [payphone-types.md](payphone-types.md) | payphone models: coin switches, coin relay, voltage and coin board settings for each |

The bench measurements of the coin relay and the patent notes are in
`Scott/coin-return-board.md` (local only, not in git).

One board, surface-mount with a few through-hole parts, that (1) fires the
HD550002C coin relay in either direction from a high voltage made on the
board, (2) reads the phone's 5 / 10 / 25 cent coin switches, and (3) talks to
Analog Redial over I2C. Options for other payphones: a relay with two coils
and a common wire (J10), 12-24 V coils (J14 off, section 6), a fourth coin
input, inputs that take a coin acceptor's logic output up to 40 V (section 8),
screw terminals beside the phone's own plugs, and a spare analog input (J13). It needs only **5 V, VIO, GND, SDA, SCL** from the
main board. VIO is the host's I2C logic voltage (3.3 V from Analog Redial,
3.0 to 5.5 V works; section 3).

## 1. Block diagram

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

## 2. Connectors

The phone has **two separate cables** for this board: a 4-wire one from the
coin switches and a 3-wire one from the coin relay. Both have a red and a
green wire, and they aren't the same wires.

| Ref | Part | Pins |
|---|---|---|
| **J6** | 5 x 2.54 mm pin header | **1 GND, 2 SCL, 3 SDA, 4 VIO, 5 +5 V**: the same pin numbers as the main board's v6 J11 (pin 4 there is +3.3 V), so a straight 1:1 cable, pin 1 to pin 1 |
| **J1** | JST XH 5-pin (B5B-XH-A, mates with XHP-5) | same five signals, same order, in parallel with J6: fit whichever suits the cable |
| **J2** | Molex KK 396, 3-pin, 0026604030 | **coin relay cable.** 1 green = COIL_A, 2 red = **not connected** (the HD550002C's own switch), 3 white = COIL_B (checked on the phone's plug; turn on Swap, section 7). **Check its voltage rating against 119 V** |
| **J11** | Phoenix MKDS 1,5/3-5,08 screw terminal | 1 COIL_A, 2 COIL_C, 3 COIL_B: for a relay without the KK 396 plug, and the only connector for a two-coil relay's common (pin 2). Fit J2 or J11 |
| **J10** | 1 x 2 pin header + shunt | **two-coil relays only:** ties COIL_C (J11 pin 2) to HV (section 7). It can't reach J2, so a relay on J2 is unaffected either way |
| **J14** | 1 x 2 pin header + shunt | **HV boost:** 20V to U3's VCC. **No shunt (the safe default): HV stays at about 19 V**, whatever the firmware does. Shunt fitted: HV goes to J7-J9's setting (section 6) |
| **J3** | Molex KK 396, 4-pin, 0026604040 | **coin switch cable.** 4 green = common (GND), 3 white = quarter, 2 red = dime, 1 blue = nickel. On the plug with its tab at the bottom, green is the left pin and blue the right |
| **J12** | KF128 screw terminal, 5 pins, 2.54 mm pitch (coin inputs are low voltage) | 1 GND, **2 coin 4**, 3 quarter, 4 dime, 5 nickel (left to right on the board: nickel, dime, quarter, coin 4, GND; nickel, dime, quarter and GND are J3's pins, in parallel). Coin 4 goes to U1 P0 |
| **J13** | 1 x 3 pin header, 2.54 mm | spare analog input: 1 VIO, 2 AIN3 (through R52 10 k, C25 100 nF to GND), 3 GND (section 9) |
| **J7-J9** | three 2-pin headers + shunts | HV setting with J14 fitted: no shunt = 52 V; any combination is valid, up to 119 V with all three (section 6) |
| **JP1** | solder jumper, **open** | **VIO = 5 V**: bridge it only for a host with no logic supply to give (a 5 V I2C host using four wires). Leave it open whenever pin 2 is wired |

**Before ordering:** with each plug in its header, check the wires land on the
pin numbers above (green on J3 pin 4, white on J2 pin 3). If one is reversed,
only that connector's pin numbers change in the schematic.

## 3. I2C and VIO

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

## 4. U1 pins (TCA9534APWR, TSSOP-16, VCC = VIO)

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
[output-stage-wiring.md](output-stage-wiring.md).

## 5. Stage 1: 5 V to 20 V (U2, MC34063ADR2G)

1.25 V reference, internal 1.5 A switch, boost topology per the datasheet
application circuit. The switch collector's absolute maximum is **40 V**, so
the 20 V output (switch node about 21 V with the diode drop) is safe.

Datasheet designators (TI SLLS636N's step-up converter figure) on this board:

| Datasheet | This board | Value here (datasheet example: 12 V in, 28 V / 175 mA out) |
|---|---|---|
| VIN | +5V | 5 V |
| RSC | R11, R45, R46 | **3 x 1 ohm in parallel = 0.33 ohm** (0.22 ohm) |
| 180 ohm, pin 8 to pin 7 (unlabeled in the figure) | R12 | **100 ohm, 1206** (180 ohm): about 40 mA of drive at 5 V, a forced beta of 25 at the 1 A limit, the datasheet example's ratio. 0.13 W |
| L | L1 | **100 uH**, SXN SMDRI127-101MT (170 uH) |
| 1N5819 | D1 | 1N5819HW (SOD-123) |
| CO | C7 | 100 uF / 50 V (330 uF) |
| CT | C8 | **1 nF** (1500 pF) |
| R1 (bottom) | R14 | **10 k, 1 %** (2.2 k) |
| R2 (top) | R13 | **150 k, 1 %** (47 k) |
| 100 uF input cap | C5, C6 | 100 uF + 100 nF (100 uF) |
| 1 uH / 100 uF "optional filter" | not fitted | |

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

**Pin 8 resistor.** The datasheet's electrical table characterizes this
connection at **VCC = 5 V with 82 ohm from pin 8 to VCC**: VCE(sat) 0.45 to 0.7 V
at 1 A (forced beta about 20). The figure's 180 ohm is for 12 V. The datasheet
warns that in this connection a hard-saturated switch can take up to 2 us to
turn off at low currents (300 mA or less) above 30 kHz. Each pulse here peaks
near 1 A (fixed L and on-time, pulses are skipped at light load), so it should
be fine, but check the switch-node waveform on the bench.

**Values for 5 V in, 20 V out, 100 mA** (the datasheet's step-up equations,
assuming 0.4 V diode drop, 1 V switch drop, about 33 kHz; the datasheet says to
verify designs on the bench):

| Item | Value | Equation / why |
|---|---|---|
| ton / toff | about 24 us / 6 us | (Vout + VF - Vin) / (Vin - Vsat) = 3.85 |
| C8 (CT) | about 1000 pF | CT = 4e-5 x ton |
| Ipk(switch) | about 1 A | 2 x Iout(max) x (ton/toff + 1) |
| R11, R45, R46 (RSC) | **3 x 1 ohm in parallel, 0.33 ohm** | 0.3 V / Ipk(switch). About 0.5 A RMS, so 0.08 W in all, under 30 mW each |
| L1 | about 100 uH, Isat > 1.5 A | L(min) = (Vin - Vsat) / Ipk x ton |
| C7 (CO) | 100 uF gives about 0.2 V p-p by the equation | CO = 9 x Iout x ton / Vripple wants about 220 uF for 100 mV; real ripple is higher with ESR (the datasheet's own 330 uF example measures 400 mV p-p) |

At 5 V the input draws about 0.4 A average with about 1 A peaks.

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
- Caveats **[verify]**: at VIO = 3.3 V a high-threshold Q1 may be tens of ohms,
  which shifts the rail by about 1-3 %; that's fine. Pin 5's allowed input range
  must include the rail during shutdown (I recall -0.3 to +40 V).

**Why have the disable.** Left running, both stages draw roughly 0.5 W (about
100 mA from the 5 V rail) continuously, and C1 stays charged around the clock.
With the disable the draw is a few mA and there's no high voltage until firmware
asks for it. The wait is small: stage 1 starts in tens of ms and stage 2 reaches
52-60 V in well under 1 s (119 V in about 1 s). A coin return or collect happens
when a call ends, so this doesn't show. Firmware can also raise EN_HV early and
drop it after about 10 s.

## 6. Stage 2: 20 V to 52-119 V (U3, UC3843BD1R2G + Q6, STN4NF20L)

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
| Dz1 | 15 V gate clamp (cathode to gate, anode to Q6 source) | **SMAZ15** (1 W) | OUT is just under the STN4NF20L's 20 V Vgs maximum, so this is margin |
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

**Sizing** (my arithmetic from the datasheet's equations; **[verify]** on the
bench):

- **Power.** With L2 = 220 uH at 65 kHz and Ipk = 0.45 A, each cycle takes
  1/2 x L x Ipk^2 x f = 1.45 W out of L2, and the boost adds what comes straight
  from the input: about **1.7 W at 119 V, 2.2 W at 60 V, 2.4 W at 52 V** from the
  20 V rail. Stage 1 supplies about 2 W (100 mA), so while C1 charges below about
  70 V, stage 2 can ask for more than stage 1 gives: see the bench checks in section 11.
- **Charge time.** C1 stores 1/2 C V^2: 0.3 J at 52 V, 0.4 J at 60 V, 1.6 J at
  119 V. At 1.7-2.4 W that's a fraction of a second up to about 1 s, so the
  firmware's 3 s wait is generous.
- **Frequency.** 1.72 / (12 k x 2.2 nF) = 65 kHz.
- **Mode: discontinuous (DCM) once HV is above about 30 V.** At the 0.45 A peak the
  switch is on for L2 x Ipk / 20 V = 5 us, and L2 empties in
  L2 x Ipk / (HV - 20 V): 2.5 us at 60 V, about 1 us at 119 V. Both sum to less than the
  15.4 us period, so the current returns to zero every cycle, with no right-half-plane
  zero and no subharmonic oscillation. Duty is only about 32 %.
- **Loop compensation.** In DCM each cycle delivers 1/2 x L2 x Ipk^2, and Ipk
  follows COMP: Ipk = (V_COMP - 1.4 V) / (3 x Rcs1), a gain of 0.15 A per volt.
  Above a very low pole the power stage is an integrator:

  G(s) = L2 x f x Ipk / (3 x Rcs1 x HV x C1 x s)

  Rz1 + C28 from COMP to FB make a Type II compensator, flat at Rz / Rfbt above
  its zero. So the crossover is

  fc = (Rz / 300 k) x L2 x f x Ipk / (2 pi x 3 x Rcs1 x HV x C1)

  | Case | Ipk | fc with Rz1 = 1 M |
  |---|---|---|
  | 60 V, full current (charging) | 0.45 A | about 40 Hz |
  | 60 V, divider load only | about 60 mA | about 5.5 Hz |
  | 130 V, full current | 0.45 A | about 18 Hz |
  | 130 V, divider load only | about 0.12 A | about 4.5 Hz |

  Loop gain falls with Ipk, so the zero (1.6 Hz) goes below the lowest crossover,
  for 70-90 degrees of phase margin in every case. C1's ESR zero (about 1 kHz) is
  far above the crossover. A low crossover suits this job: C1 is large, the load is
  one relay pulse, and while charging the loop is pinned at the current limit.
- **C1 droop.** A pulse of 100 mA for 50 ms (5 mC) drops it about 23 V. Measure
  the relay's pull-in current and resize C1 if needed.

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

## 7. Firing and polarity

See [output-stage-wiring.md](output-stage-wiring.md) for the wiring.

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

## 8. Coin inputs

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

## 9. Measuring HV, 20 V and firing current (U4, ADS1115)

U1 has no ADC, so U4 does the measuring (16-bit, 4 channels; its wiring table is in
[output-stage-wiring.md](output-stage-wiring.md)). R44 (1 k) sits between R26 and AIN2,
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

## 10. Cable from the main board

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
  an I2C buffer (TCA4307 or PCA9517 class; I haven't checked stock) at the main-board end
  so the cable can't load the codec's bus, or a differential extender such as the PCA9615
  for longer runs. **Probably not needed under 1 m.**

## 11. Open items

**Before ordering**

1. **Connector pin order:** check J2 and J3 against the real plugs (section 2), and J2's
   voltage rating.
2. **Finish the PCB:** see "Still to do" in [coin-board-smt.md](coin-board-smt.md).
3. **U4's footprint:** `Custom:ADS1115 Module` numbers its pads 1-10 along the header, but the
   symbol's pin numbers are the bare chip's, so as it stands every module pin lands on the wrong
   net (the VDD pad gets GND). See "Still to do" in [coin-board-smt.md](coin-board-smt.md).
4. **Datasheet checks** for the **[verify]** items, above all U2 (timing cap, boost
   equations), U3 (Rcs1, Rt/Ct, the divider), D3's clamp voltage, and K1's contact rating.

**On the bench**

5. **Which voltage the phone needs:** it works at about 50-60 V; J14 alone gives 52 V and
   J14 + J9 gives 60 V. The higher settings are margin for other phones.
6. **Stage 1 against stage 2:** while charging, stage 2 can ask for more than stage 1's 2 W
   (section 6). The rail may sag, and if it reaches U3's 7.6 V stop, the converter hiccups.
   If it does, raise Rcs1 for a lower peak current, or add a soft start on COMP.
7. **Overshoot at the end of a charge:** C28 winds up while the error amplifier is saturated;
   watch that HV stays below D3's standoff at 119 V. A smaller C28 (47 nF, zero at 3.4 Hz) cuts
   overshoot at the cost of light-load phase margin. Audible chirping from skipped cycles at
   light load is harmless.
8. **U2's switch-node waveform** (section 5), and whether charging pulls the 5 V rail below spec.
9. **Pull-in charge** (current x time at the operating voltage), to size C1.
10. **J14 off:** that U3 stays off with no supply (R53 holding Q6), and HV settles near 19 V.
11. **Coin inputs through D16-D19:** the low level U1 reads with a real switch (about
    0.3 V diode drop plus the 1 k), and a coin acceptor's output if one is to hand.

**Firmware**

12. **Written, tested only against simulated chips:** `firmware/components/coinboard/` drives U1
    and U4, counts coins, and fires the relay with the sequence and limits of section 7
    ([payphone-firmware-plan.md](payphone-firmware-plan.md) has the details and the bring-up
    steps). On the first board, check it against the real thing: the coin inputs' timing, U4's
    readings against a meter, the pulse length the relay needs, and the overcurrent limit.

## Sources

- [TCA9534A datasheet](https://www.ti.com/lit/ds/symlink/tca9534a.pdf)
- [Panasonic TX2-5V (DigiKey)](https://www.digikey.com/en/products/detail/panasonic-industry/TX2-5V/251803); the Omron G6S-2 DC5 fits the same footprint
- [ES2D (DigiKey)](https://www.digikey.com/en/products/detail/smc-diode-solutions/ES2D/6022516)
- [STN4NF20L (ST)](https://www.st.com/en/power-transistors/stn4nf20l.html)
- [STN4NF20L (DigiKey)](https://www.digikey.com/en/products/detail/stmicroelectronics/STN4NF20L/2440279)
- [Molex 0026604040 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604040/197447)
- [Molex 0026604030 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604030/96502)
