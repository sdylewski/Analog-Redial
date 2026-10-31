# Payphone coin board: design

**Nothing here is built or simulated.** Values marked **[verify]** come from
distributor listings or my memory of a datasheet, not from the manufacturer's
datasheet; check them before ordering. Reference designators match
`Payphone_v1/Payphone_v1.kicad_sch`.

| Doc | Covers |
|---|---|
| This one | how the board works: connectors, logic, the two power stages, coin inputs, cable, open items |
| [output-stage-wiring.md](output-stage-wiring.md) | relay driver, polarity relay and firing switch, pin by pin |
| [coin-board-smt.md](coin-board-smt.md) | parts list, what JLCPCB places, PCB layout and routing |

The bench measurements of the coin relay and the patent notes are in
`Scott/coin-return-board.md` (local only, not in git).

One board, surface-mount with a few through-hole parts, that (1) fires the
HD550002C coin relay in either direction from a high voltage made on the
board, (2) reads the phone's 5 / 10 / 25 cent coin switches, and (3) talks to
Analog Redial over I2C. It needs only **5 V, VIO, GND, SDA, SCL** from the
main board. VIO is the host's I2C logic voltage (3.3 V from Analog Redial,
3.0 to 5.5 V works; section 3).

## 1. Block diagram

```
 Analog Redial                         COIN BOARD
 (v6 accessory hdr)                                            HV zone (up to 119 V)
 +5V  --- J6.1 ---------> [U2 MC34063 5V->20V] --> 20V --> [U3 UC3843 + Qa1 + L2 + D2] --> HV (C1)
 VIO  --- J6.2                  ^ Q1                                                        |
 SDA  --- J6.3 --+              |                                                           |
 SCL  --- J6.4 --+--> [U1 TCA9534A @0x39] P4 EN_HV                                          |
 GND  --- J6.5   |                        P5 POL  --> Q2 --> K1 (DPDT relay) <--------------+
                 |                        P6 FIRE --> Q4, Q5 --> Q3 (low side) --> R26 --> GND
                 |                        P0-P2 <-- coin switches <-- J3
                 +--> [U4 ADS1115 @0x48]  HV, 20V, firing current

 K1 poles --> J2 --> coin relay HD550002C (white, green wires).
```

## 2. Connectors

The phone has **two separate cables** for this board: a 4-wire one from the
coin switches and a 3-wire one from the coin relay. Both have a red and a
green wire, and they aren't the same wires.

| Ref | Part | Pins |
|---|---|---|
| **J6** | 5 x 2.54 mm pin header | **1 +5 V, 2 VIO, 3 SDA, 4 SCL, 5 GND**: the main board's v6 J6 order, so a straight 1:1 cable |
| **J1** | JST EH 5-pin (B5B-EH-A) | same five signals, same order, in parallel with J6: fit whichever suits the cable |
| **J2** | Molex KK 396, 3-pin, 0026604030 | **coin relay cable.** 1 white = COIL_A, 2 red = not connected, 3 green = COIL_B. **Check its voltage rating against 119 V** |
| **J3** | Molex KK 396, 4-pin, 0026604040 | **coin switch cable.** 4 green = common (GND), 3 white = quarter, 2 red = dime, 1 blue = nickel. On the plug with its tab at the bottom, green is the left pin and blue the right |
| **J7-J9** | three 2-pin headers + shunts | HV setting: no shunt = 52 V; any combination is valid, up to 119 V with all three (section 6) |
| **JP1** | solder jumper, **open** | **VIO = 5 V**: bridge it only for a host with no logic supply to give (a 5 V I2C host using four wires). Leave it open whenever pin 2 is wired |

**Before ordering:** with each plug in its header, check the wires land on the
pin numbers above (green on J3 pin 4, white on J2 pin 1). If one is reversed,
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
board has none.

**VIO: the logic supply comes from the host.** U1 and U4 run from J6 pin 2,
not from a regulator on this board. An I2C input must not go above its chip's
supply (+0.6 V absolute maximum) nor read high below 0.7 x its supply, so the
chips have to run at the host's own logic level. Taking it from the host does
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
| 4 | P0 | Nickel | in | high | 10 k pull-up to VIO, 1 k series, 100 nF on the switch side |
| 5 | P1 | Dime | in | high | same |
| 6 | P2 | Quarter | in | high | same |
| 7 | P3 | spare | | | not connected |
| 8 | GND | GND | | | |
| 9 | P4 | **EN_HV** | out | low (R1 100 k) | turns on stage 1, and so stage 2: HV exists only while high |
| 10 | P5 | **POL** | out | low (R2 100 k) | drives Q2 and K1: sets which way the coil is driven |
| 11 | P6 | **FIRE** | out | low (R3 10 k) | drives Q3: current through the coil |
| 12 | P7 | spare | | | not connected |
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
| D8 / D9 / D10 (R34-R36) | VIO -> 1.5 k -> LED -> NICKEL_SW / DIME_SW / QUARTER_SW (the J3 side of R7-R9) | that coin switch is closed |
| D11 / D12 / D13 (R37-R39) | EN_HV / POL / FIRE -> 1.5 k -> LED -> GND | that output is high |

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
| RSC | R11 | **0.33 ohm, 0.5 W** (0.22 ohm) |
| 180 ohm, pin 8 to pin 7 (unlabeled in the figure) | R12 | **82 ohm** (180 ohm) |
| L | L1 | **100 uH**, Würth 7447471101 (170 uH) |
| 1N5819 | D1 | 1N5819HW (SOD-123) |
| CO | C7, C9 | 2 x 22 uF / 35 V (330 uF) |
| CT | C8 | **1 nF** (1500 pF) |
| R1 (bottom) | R14 | **10 k, 1 %** (2.2 k) |
| R2 (top) | R13 | **150 k, 1 %** (47 k) |
| 100 uF input cap | C5, C6 | 22 uF + 100 nF (100 uF) |
| 1 uH / 100 uF "optional filter" | not fitted | |

| Pin | Name | Net / connection |
|---|---|---|
| 1 | SwC (switch collector) | The switch node: L1 (far end) and D1 anode. Switch on: current flows +5V -> R11 -> L1 -> pin 1 -> pin 2 -> GND. Off: the inductor kick forward-biases D1 and charges C7/C9 |
| 2 | SwE (switch emitter) | **Straight to GND** |
| 3 | TC (timing cap) | C8 to GND. Nothing else on this pin |
| 4 | GND | GND |
| 5 | Vfb | Divider midpoint: R13 from 20V, R14 to the **Q1 drain** (Q1 source = GND, gate = EN_HV) |
| 6 | Vcc | +5V (the input side of R11), with C5/C6 to GND |
| 7 | Ipk (peak current sense) | The L1 side of R11. The cycle ends when the drop across R11 reaches about 330 mV |
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
| R11 (RSC) | **0.33 ohm, 0.5 W** | 0.3 V / Ipk(switch) |
| L1 | about 100 uH, Isat > 1.5 A | L(min) = (Vin - Vsat) / Ipk x ton |
| C7, C9 (CO) | 2 x 22 uF gives about 0.5 V p-p by the equation | CO = 9 x Iout x ton / Vripple wants about 220 uF for 100 mV; real ripple is higher with ESR (the datasheet's own 330 uF example measures 400 mV p-p) |

At 5 V the input draws about 0.4 A average with about 1 A peaks.

**Enable.** The MC34063 has no enable pin, so **Q1 (2N7002) sits in the bottom
leg of the feedback divider**:

```
  20V ---[ R13 150k ]---+--- U2 pin 5 (Vfb)
                        |
                   [ R14 10k ]
                        |
                     Q1 drain
  EN_HV --- Q1 gate     (R1 100k pulldown on EN_HV)
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

## 6. Stage 2: 20 V to 52-119 V (U3, UC3843BD1R2G + Qa1, IRF630)

Current-mode PWM controller, non-synchronous boost, driving an external N-FET.
It starts at about 8.4 V (7.8 to 9 V) and stops at 7.6 V, so it runs from the
20 V rail and stage 1 must start first. The UCx843 runs to about 97 % duty; the
UCx844/5 are limited to 50 % and wouldn't work.

| Ref | Function | Value | Notes |
|---|---|---|---|
| **Power stage** | | | |
| L2 | Boost inductor (20V to SW2) | **220 uH**, Würth 7447471221 | shielded, 1.4 A Isat, 0.51 ohm |
| Qa1 | Switch (drain SW2, source to Rcs1) | **IRF630PBF** (TO-220, 200 V, 9 A) | |
| D2 | Output diode (SW2 to HV) | **ES2D** (SMA, 200 V, 2 A ultrafast) | cathode toward C1. Average current is only tens of mA |
| C1 | HV output capacitor | **220 uF / 200 V** | Nichicon UCS2D221MHD, 18 x 26.5 mm, 7.5 mm leads |
| C14, C15 | Input decoupling on 20V | **2 x 22 uF / 35 V** | close to L2 |
| **Gate drive** | | | |
| Rg1 | U3 OUT to Qa1 gate | **100 ohm** | damps ringing and limits the current into Dz1: OUT swings to about 18.5 V, so Dz1 sees about 35 mA each on-time; still switches the IRF630 in about 0.3 us |
| Dz1 | 15 V gate clamp (cathode to gate, anode to Qa1 source) | **SMAZ15** (1 W) | OUT is just under the IRF630's 20 V Vgs maximum, so this is margin |
| **Current sense** | | | |
| Rcs1 | Qa1 source to GND | **2 ohm, 1 W** (2512) | Ipk = 1 V / Rcs1 = 0.5 A |
| Rf1 | Rcs1 to CS pin 3 | **1 k** | with Cf1, filters the leading-edge spike (the UC3843 has no blanking) |
| Cf1 | CS pin 3 to GND | **470 pF, C0G** | 0.47 us, well under the 15 us period |
| Rslope1 | RT/CT ramp to CS, in series with Cslope1 | **27 k, fit only if needed** | The converter runs in discontinuous mode (below), where subharmonic oscillation can't happen. Fit it if the switch node shows alternating long and short pulses |
| Cslope1 | DC block in the same path | **1 nF** | passes only the ramp |
| **Oscillator** | | | |
| R18 (Rt) | VREF pin 8 to RT/CT pin 4 | **12 k** | fOSC = 1.72 / (Rt x Ct) = 65 kHz |
| Ct1 | RT/CT pin 4 to GND | **2.2 nF, C0G** | sets the clock, so no X7R. Never below about 1000 pF |
| **Feedback** | | | |
| Rfbt1-3 | HV to FB pin 2 | **3 x 100 k in series** (1206) | about 40 V each |
| Rfbb1-4 | FB pin 2 to GND | **Rfbb4 15 k always fitted**; Rfbb3 100 k, Rfbb2 30 k, Rfbb1 22 k added in parallel by J9 / J8 / J7 | table below |
| Rz1 | COMP pin 1 to FB pin 2, in series with Cz1 | **1 M** | mid-band gain Rz / Rfbt = 3.3 |
| Cz1 | In series with Rz1 | **150 nF**, X7R | zero at 1.1 Hz |
| Cp | COMP pin 1 to FB pin 2, across Rz1 + Cz1 | **100 pF, C0G** | pole at 1.6 kHz, keeps switching noise out of the error amplifier. **Not on the schematic yet** (section 11) |
| **Decoupling** | | | |
| Cvcc1 | VCC pin 7 to GND | **0.1 uF, 50 V** | on the 20 V rail, so not a 16 or 25 V part |
| Cvref1 | VREF pin 8 to GND | **0.1 uF** | required by the datasheet |
| **Protection** | | | |
| D3 | Clamp across C1 | **SMBJ130A** (SMB) | 130 V standoff; cathode to HV |
| Rbl1-3 | Bleed across C1 | **3 x 1 M in series** (1206) | section below. 4.7 mW at 119 V, about 40 V each |

| Pin | Name | Net / connection |
|---|---|---|
| 1 | COMP | E/A output. Rz1 + Cz1 to pin 2. No shutdown here: U3 has no VCC whenever stage 1 is disabled |
| 2 | FB | Divider midpoint. Internal reference is **2.5 V**, so HV = 2.5 x (1 + Rfbt / Rfbb) |
| 3 | CS | Rf1 from the top of Rcs1; Cf1 to GND. The cycle ends at **1 V** |
| 4 | RT/CT | R18 from pin 8, Ct1 to GND |
| 5 | GND | GND |
| 6 | OUT | Qa1 gate through Rg1 |
| 7 | VCC | 20V, Cvcc1 to GND. 30 V absolute maximum, about 11 to 17 mA running |
| 8 | VREF | **5 V** reference; it only feeds R18 here. Cvref1 to GND |

**Voltage select.** Each shunt adds a resistor in parallel with Rfbb4, which
lowers Rfbb and raises HV. **Every combination is valid:**

| Shunts fitted | Rfbb total | HV (about) |
|---|---|---|
| none | 15 k | **52 V**, the default |
| J9 | 13.0 k | 60 V |
| J8 | 10 k | 78 V |
| J9 + J8 | 9.1 k | 85 V |
| J7 | 8.9 k | 87 V |
| J9 + J7 | 8.2 k | 94 V |
| J8 + J7 | 6.9 k | 112 V |
| J9 + J8 + J7 | 6.4 k | **119 V**, the maximum |

No shunt mistake can go above 119 V, below D3's 130 V standoff. A missing, lifted
or dirty shunt gives a lower voltage, never off or higher. An open Rfbb4 turns the
converter off (FB then sits at HV). An open Rfbt would let the output run away:
that's the failure D3 is there for.

**Sizing** (my arithmetic from the datasheet's equations; **[verify]** on the
bench):

- **Power.** With L2 = 220 uH at 65 kHz and Ipk = 0.5 A, each cycle takes
  1/2 x L x Ipk^2 x f = 1.8 W out of L2, and the boost adds what comes straight
  from the input: about **2.2 W at 119 V, 2.7 W at 60 V, 2.9 W at 52 V** from the
  20 V rail. Stage 1 supplies about 2 W (100 mA), so while C1 charges, stage 2 can
  ask for more than stage 1 gives: see the bench checks in section 11.
- **Charge time.** C1 stores 1/2 C V^2: 0.3 J at 52 V, 0.4 J at 60 V, 1.6 J at
  119 V. At about 2 W that's a fraction of a second up to about 1 s, so the
  firmware's 3 s wait is generous.
- **Frequency.** 1.72 / (12 k x 2.2 nF) = 65 kHz.
- **Mode: discontinuous (DCM) once HV is above about 31 V.** At the 0.5 A peak the
  switch is on for L2 x Ipk / 20 V = 5.5 us, and L2 empties in
  L2 x Ipk / (HV - 20 V): 2.75 us at 60 V, about 1 us at 119 V. Both sum to less than the
  15.4 us period, so the current returns to zero every cycle, with no right-half-plane
  zero and no subharmonic oscillation. Duty is only about 36 %.
- **Loop compensation.** In DCM each cycle delivers 1/2 x L2 x Ipk^2, and Ipk
  follows COMP: Ipk = (V_COMP - 1.4 V) / (3 x Rcs1), a gain of 1/6 A per volt.
  Above a very low pole the power stage is an integrator:

  G(s) = L2 x f x Ipk / (3 x Rcs1 x HV x C1 x s)

  Rz1 + Cz1 from COMP to FB make a Type II compensator, flat at Rz / Rfbt above
  its zero. So the crossover is

  fc = (Rz / 300 k) x L2 x f x Ipk / (2 pi x 3 x Rcs1 x HV x C1)

  | Case | Ipk | fc with Rz1 = 1 M |
  |---|---|---|
  | 60 V, full current (charging) | 0.5 A | about 50 Hz |
  | 60 V, divider load only | about 60 mA | about 6 Hz |
  | 130 V, full current | 0.5 A | about 22 Hz |
  | 130 V, divider load only | about 0.12 A | about 5 Hz |

  Loop gain falls with Ipk, so the zero (1.1 Hz) goes below the lowest crossover,
  for 80-90 degrees of phase margin in every case. C1's ESR zero (about 1 kHz) is
  far above the crossover. A low crossover suits this job: C1 is large, the load is
  one relay pulse, and while charging the loop is pinned at the current limit.
- **C1 droop.** A pulse of 100 mA for 50 ms (5 mC) drops it about 23 V. Measure
  the relay's pull-in current and resize C1 if needed.

**D3 and the bleed path, across C1:**

```
  HV ----+-------+-------+-----------+-----------+-----------+
         |       |       |           |           |           |
        C1      D3     Rfbt1-3     Rbl1-3       R32        R27, R28
      220uF   SMBJ130A  3x 100k     3x 1M      1.18M       D5, D7
      /200V              |           |           |         (HV LED)
         |       |       +-- FB      |         +-- AIN0      |
         |       |       |           |         R33           |
         |       |     Rfbb4         |          33k          |
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
  only drains C1 while Rfbb4 is connected; if that opens, R32, Rbl1-3 and the LED
  string still reach 20 V in about 88 s. (A failed-open Rfbt runs the output up to
  D3, so that case is covered by the clamp, not the bleed.)

## 7. Firing and polarity

See [output-stage-wiring.md](output-stage-wiring.md) for the wiring.

**Sequence** (firmware): set POL, raise EN_HV, wait until HV is up (U4 reads it,
or a fixed delay of about 3 s), pulse FIRE, drop FIRE, drop EN_HV. **Never change
POL while FIRE is high or while current flows**, so K1's contacts only switch
dead. Cap FIRE at about 1 s. The relay's own switch (red-green) cuts the current
by itself, so FIRE doesn't need exact timing.

## 8. Coin inputs

- **The coin switches are passive:** green is the common, and green to white / red
  / blue closes briefly for a quarter / dime / nickel. There's no supply to them:
  green goes to GND, and each line is pulled up to VIO.
- **Each line:** R4-R6 10 k pull-up to VIO, R7-R9 1 k series to U1, C2-C4 100 nF to
  GND on the J3 side. D8-D10 show each line (section 4).
- Each closure pulls its line low (active low). The contacts carry about 0.3 mA from
  the pull-up plus about 0.5 mA through the line's debug LED, enough wetting current
  for old contacts.
- Mechanical contacts bounce. The RC on each line (about 1 ms) takes out the fastest
  spikes; firmware polls U1 about every 2-5 ms, debounces for a few ms and counts
  edges.

## 9. Measuring HV, 20 V and firing current (U4, ADS1115)

U1 has no ADC, so U4 does the measuring (16-bit, 4 channels; its wiring table is in
[output-stage-wiring.md](output-stage-wiring.md)). Firmware can check "reached the
target within N s", and see **C1 sag after a fire**: if the relay doesn't fire the
voltage stays up; if it does, it sags by I x t / C (about 20 V for 100 mA x 50 ms).
U4 can be left off: firmware then falls back on a fixed delay and knows nothing about
whether the relay fired.

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
2. **Update PCB from Schematic** hasn't been run since these changes, and the board file
   still has the MCP23008: see "Still to do" in [coin-board-smt.md](coin-board-smt.md).
3. **Datasheet checks** for the **[verify]** items, above all U2 (timing cap, boost
   equations), U3 (Rcs1, Rt/Ct, the divider), D3's clamp voltage and K1's contact rating.
4. **Not on the schematic yet, needed:**
   - **Cp**, 100 pF C0G, COMP to FB (section 6).
5. **Not on the schematic, your call:**
   - **A fuse or PTC on +5V.** There's none, so a stage-1 fault loads the main board's 5 V
     directly. It would need about 1 A hold (Littelfuse RXEF110, radial).
   - **At the cable entry:** 100 ohm series resistors in SDA and SCL, ESD parts on each line
     (the cable is an antenna for static in a phone), and about 100 uF on +5V.
   - **DNP 4.7 k I2C pull-ups**, for a host without its own.
   - **A header for the spare pins** P3 and P7.
   - **Filter caps at U4's inputs and a 100 nF 250 V cap across stage 2's output**
     (routing notes in [coin-board-smt.md](coin-board-smt.md)).

**On the bench**

6. **Which voltage the phone needs:** it works at about 50-60 V; no shunt (52 V) is the
   default and J9 gives 60 V. The higher settings are margin for other phones.
7. **Stage 1 against stage 2:** while charging, stage 2 can ask for more than stage 1's 2 W
   (section 6). The rail may sag, and if it reaches U3's 7.6 V stop, the converter hiccups.
   If it does, raise Rcs1 for a lower peak current, or add a soft start on COMP.
8. **Overshoot at the end of a charge:** Cz1 winds up while the error amplifier is saturated;
   watch that HV stays below D3's standoff at 119 V. A smaller Cz1 (47 nF, zero at 3.4 Hz) cuts
   overshoot at the cost of light-load phase margin. Audible chirping from skipped cycles at
   light load is harmless.
9. **U2's switch-node waveform** (section 5), and whether charging pulls the 5 V rail below spec.
10. **Pull-in charge** (current x time at the operating voltage), to size C1.

**Firmware**

11. A TCA9534A driver (shared with the main board v6 expander), coin events, and a coin-relay
    output (return/collect) with a safety timeout. **None of it exists yet.**

## Sources

- [TCA9534A datasheet](https://www.ti.com/lit/ds/symlink/tca9534a.pdf)
- [Omron G6S-2 5 VDC (Conrad)](https://www.conrad.com/en/p/omron-g6s-2-5-vdc-pcb-relay-5-v-dc-2-a-2-change-overs-1-pc-s-503840.html)
- [ES2D (DigiKey)](https://www.digikey.com/en/products/detail/smc-diode-solutions/ES2D/6022516)
- [STN4NF20L (ST)](https://www.st.com/en/power-transistors/stn4nf20l.html)
- [STN4NF20L (DigiKey)](https://www.digikey.com/en/products/detail/stmicroelectronics/STN4NF20L/2440279)
- [Molex 0026604040 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604040/197447)
- [Molex 0026604030 (DigiKey)](https://www.digikey.com/en/products/detail/molex/0026604030/96502)
