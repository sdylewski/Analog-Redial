# Payphone coin board: schematic and parts (draft 1)

Private, untracked. 2026-10-25. Companion to
[coin-return-board.md](coin-return-board.md) (measurements, patent, converter
analysis). **Nothing here is built or simulated.** Values marked **[verify]**
come from distributor listings or my memory of a datasheet, not from the
manufacturer's datasheet; check them before ordering.

**Start here for wiring:** [output-stage-wiring.md](output-stage-wiring.md)
(relay driver, polarity relay, firing switch: pin-by-pin tables against the
Payphone_v1 schematic). **THT vs. SMT parts:** [coin-board-smt.md](coin-board-smt.md).

One board that (1) fires the HD550002C coin relay in either direction from
a high-voltage supply made on the board, (2) reads the coin sensor's
5 / 10 / 25 cent lines, and (3) talks to Analog Redial over I2C. It needs
only **5 V, 3.3 V, GND, SDA, SCL** from the main board.

## 1. Block diagram

```
 Analog Redial                         COIN BOARD
 (v6 accessory hdr)                                            HV zone (up to 119 V)
 +5V  --- J1.1 --+-- F1 --+--> [U2 MC34063A 5V->20V] --> +20V --> [U3 UC3843A + Qa IRF630 + L2 + D2] --> HV+ (Cout)
 +3V3 --- J1.2   |        |         ^EN                          ^COMP (shutdown)                    |
 SDA  --- J1.3 --+        +-> sensor +5V (J3, JP1)                    |                              |
 SCL  --- J1.4 --+--> [U1 MCP23008 @0x20] GP4 HV_EN ----------------+                              |
 GND  --- J1.5      |        GP5 POL --> Q2 (2N7000) --> K1 (DPDT relay) <-- HV+ -------------------+
 (J1 = main board v6 J6)|  GP6 FIRE --> Q3 (IRF630, low side) --> Rsense --> GND
                    |        GP0..2 <-- coin inputs <-- J3 (coin sensor)
                    |        GP3, GP7: spare, to J5

 K1 poles --> J2 (coil: A / B).  Relay on the coil: HD550002C, white + green wires.
```

## 2. Connectors

| Ref | Pins | Function |
|---|---|---|
| **J1** | 5 x 2.54 mm; mates with the main board's **v6 J6** | **1 +5 V, 2 +3.3 V, 3 SDA, 4 SCL, 5 GND**, the same order as J6, so a straight 1:1 cable |
| **J2** | **Molex KK 396, 3-pin header, 0026604030** | the relay's plug: A = white, B = green, red unconnected (so the original plug fits). **Check its voltage rating against 119 V (130 V for margin)** |
| **J3** | **Molex KK 396, 4-pin header, 0026604040** | coin sensor. **The sensor has 5 wires but this is a 4-pin plug: identify which wires are used**; if five are needed, a 5-pin KK (26-60-4050) is the next size. Intended use: +V, GND, NICKEL, DIME, QUARTER. **The sensor's 5 wires (white, blue, red, green, white) are not yet identified**; measure it first (patent says supply 3-9 V and three open-collector outputs) |
| **J7-J9** | three 2-pin headers, **HV profile select** | no jumper = 52 V; any combination of jumpers is a valid setting, up to 119 V with all three (section 5) |
| **J5** | 1 x 4 | GP3, GP7 (spare I/O), 3V3, GND |
| JP1 | solder jumper | sensor supply: +5 V (default) or an external supply on J3 |

## 3. I2C devices and the main board

| Device | Address | Notes |
|---|---|---|
| ES8388 (on the main board) | 0x10 | already there |
| **U1 MCP23008-E/P** (DIP-18) | **0x20** (A0-A2 wired directly to GND) | VDD = 3.3 V; /RESET tied to 3.3 V via 10 k; I/O pins default to inputs at power-up, so every output below has a pulldown |

The main board already has the I2C pull-ups; add **DNP** 4.7 k pads on this
board in case it is used standalone.

## 4. Pin assignment (MCP23008, VDD = 3.3 V)

**U1 pin connections (MCP23008-E/P, DIP-18):**

| DIP pin | Name | Net / connection |
|---|---|---|
| 1 | SCL | I2C clock (from J1 pin 4) |
| 2 | SDA | I2C data (from J1 pin 3) |
| 3 | A2 | GND (address bit) |
| 4 | A1 | GND (address bit) |
| 5 | A0 | GND (address bit) -> address **0x20** |
| 6 | ~RESET | +3.3 V via 10 k pull-up (always enabled) |
| 7 | NC | not connected (no internal bond) |
| 8 | INT | unused (active-low open-drain interrupt output) |
| 9 | VSS | GND |
| 10 | GP0 | NICKEL (coin sensor input) |
| 11 | GP1 | DIME |
| 12 | GP2 | QUARTER |
| 13 | GP3 | SPARE_A (to J5) |
| 14 | GP4 | HV_EN (output, 100 k pulldown) |
| 15 | GP5 | POL (output, 100 k pulldown) |
| 16 | GP6 | FIRE (output, 10 k pulldown) |
| 17 | GP7 | SPARE_B (to J5) |
| 18 | VDD | +3.3 V |

**GPIO functional assignment:**

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

### Stage 1: 5 V to 20 V, U2 = MC34063AP (DIP-8)

1.25 V reference, internal 1.5 A switch, boost topology per the datasheet
application circuit. The switch transistor's collector (pin 1/2) absolute
maximum is **40 V**, so the 20 V output (switch node about 21 V with the diode
drop) is safe.

**U2 pin connections (MC34063AP, DIP-8), using the designators of the datasheet's step-up converter figure** (TI SLLS636N calls it Figure 10; some copies number it 7-1). Datasheet name -> this board:

| Datasheet | This board | Value here (datasheet example: 12 V in, 28 V / 175 mA out) |
|---|---|---|
| VIN | +5 V after F1 | 5 V |
| RSC | R11 | **0.33 ohm, 0.5 W** (0.22 ohm) |
| 180 ohm, pin 8 to pin 7 (**unlabeled** in the figure) | R12 | **82 ohm, 0.25 W** (180 ohm). The schematic has 68 ohm: also fine (a little more base drive) |
| L | L1 | about 100 uH (170 uH) |
| 1N5819 | D1 | 1N5819 |
| CO | C7, C9 | 2 x 22 uF / 35 V (330 uF) |
| CT | C8 | about 1000 pF (1500 pF) |
| R1 (bottom) | R14 | **10 k, 1 %** (2.2 k) |
| R2 (top) | R13 | **150 k, 1 %** (47 k) |
| 100 uF input cap | C5, C6 | 22 uF / 10 V + 100 nF (100 uF) |
| 1 uH / 100 uF "optional filter" | not fitted | |

In this section the datasheet's R1/R2 are U2's divider, R14 (bottom) and R13 (top) on the schematic.

| Pin | Name | Net / connection |
|---|---|---|
| 1 | SwC (switch collector) | The switch node: L (far end) and D1 anode. Switch ON: current flows VIN -> RSC -> L -> pin 1 -> pin 2 -> GND. OFF: inductor kick forward-biases D1 and charges CO |
| 2 | SwE (switch emitter) | **Straight to GND** |
| 3 | TC (timing cap) | CT to GND. Nothing else on this pin |
| 4 | GND | GND |
| 5 | Vfb (comparator inv. input) | Divider midpoint: R13 from the +20 V rail, R14 to the **Q5 drain** (Q5 source = GND, gate = HV_EN). Q5 on = regulates; Q5 off = R14 open, pin 5 floats up to the rail, converter stops |
| 6 | Vcc | VIN (the input side of RSC), with C_in to GND |
| 7 | Ipk (peak current sense) | The **L side of RSC** (L's near end connects here too). The cycle ends when the drop across RSC (pin 6 minus pin 7) reaches about 330 mV |
| 8 | DC (driver collector) | Through **R12** to pin 7 |

  ```
  Stage 1 boost topology (datasheet designators, MC34063AP pin numbers):

                  RSC              L                  D1 (1N5819)
  +5V --+--+--/\/\/\--+--+--[100uH]--+-- pin 1 ----->|----+---> +20V out
        |  |          |  |           (SwC)                |
        | pin 6     pin 7 +--R12 82R--- pin 8    pin 5 <--+-- R13 150k
        | (Vcc)    (Ipk)                (DC)   (Vfb)  |         |
      C_in                                           +-- R14 10k
     22uF/10V                                         |        |
     +100nF       pin 2 (SwE) ---> GND          Q5 drain    CO 2x22uF/35V -> GND
        |                                      (2N7000 pin 3)
       GND         pin 3 (TC) --- CT --- GND   Q5 gate (pin 2) = HV_EN
                   pin 4 (GND) --- GND         Q5 source (pin 1) = GND

  D1 anode  = pin 1 / L junction;  cathode = +20V rail (band faces output)
  ```

**Feedback.** Vout = 1.25 x (1 + R13/R14) (the formula printed on the figure). **R13 = 150 k, R14 = 10 k, both 1 %**, give 1.25 x 16 = **20.0 V**. Only the ratio of 15 matters (33 k / 2.2 k also works).

**Pin 8 resistor.** The datasheet's electrical table characterizes this connection at **VCC = 5 V with 82 ohm from pin 8 to VCC**: VCE(sat) 0.45 to 0.7 V at 1 A (forced beta about 20). So 82 ohm is the right value for 5 V in; the figure's 180 ohm is for 12 V. The alternative is the plain Darlington connection (pin 8 tied to pin 1, no resistor), which is simplest but has VCE(sat) 1 to 1.3 V at 1 A. The datasheet warns that in the resistor connection a hard-saturated switch can take up to 2 us to turn off at low currents (300 mA or less) above 30 kHz. Here each pulse peaks near 1 A (fixed L and on-time, pulses are skipped at light load), so it should be fine, but check the switch-node waveform on the bench.

**Other values for 5 V in, 20 V out, 100 mA** (the datasheet's step-up equations, assuming 0.4 V diode drop, 1 V switch drop, about 33 kHz; the datasheet's own text says to verify designs on the bench):

| Item | Value | Equation / why |
|---|---|---|
| ton / toff | about 24 us / 6 us | (Vout + VF - Vin) / (Vin - Vsat) = 3.85 |
| CT | about 1000 pF | CT = 4e-5 x ton |
| Ipk(switch) | about 1 A | 2 x Iout(max) x (ton/toff + 1) |
| RSC | **0.33 ohm, 0.5 W** | 0.3 V / Ipk(switch) |
| L | about 100 uH, Isat > 1.5 A | L(min) = (Vin - Vsat) / Ipk x ton |
| CO | 2 x 22 uF gives about 0.5 V p-p by the equation | CO = 9 x Iout x ton / Vripple wants about 220 uF for 100 mV; real ripple is higher with ESR (the datasheet's own 330 uF example measures 400 mV p-p) |

At 5 V the input draws about 0.4 A average with about 1 A peaks, so the **500 mA PTC (F1) is too small**; size it for about 1 A hold.

- **C_in:** 22 uF / 10 V electrolytic + 100 nF ceramic, at pin 6 to GND.
  Footprint: **`Capacitor_THT:CP_Radial_D5.0mm_P2.00mm`**.
- **CO (Cout1):** 2 x 22 uF / 35 V electrolytic, at the +20 V output to GND.
  Footprint: **`Capacitor_THT:CP_Radial_D5.0mm_P2.00mm`** (a 22 uF / 35 V
  is typically 5 mm diameter; if the specific part is 6.3 mm, use
  `CP_Radial_D6.3mm_P2.50mm` — check the datasheet).
- **L (L1)** = 47-100 uH, Isat >= 2 A (larger than the MT3608 design because the
  MC34063A switches at only about 30-100 kHz). **[verify]** sizing with the
  datasheet equations. **D1** = **1N5819** (DO-41, Schottky, 40 V, 1 A).
- Feedback: Vref = 1.25 V, Vout = 1.25 x (1 + R13 / R14): **R13 = 150 k,
  R14 = 10 k gives 20.0 V**.
- Timing capacitor (CT, pin 3): sets the switching frequency; **[verify]**
  value from the datasheet's boost design procedure.
- 5 V to 20 V is 4x, about 75 % duty, fine for this controller.

**Enable (EN), redesigned.** The MC34063A has no enable pin. The earlier
idea (a 2N7000 shorting CT) had two problems: an N-FET turns on when its gate
is *high*, so HV_EN would have needed an inverter, and holding CT low can leave
the internal switch stuck on. Instead, **put Q5 (2N7000, TO-92) in the
bottom leg of the feedback divider**:

```
  +20V rail ---[ R13 150k ]---+--- U2 pin 5 (Vfb)
                             |
                        [ R14 10k ]
                             |
                      Q5 drain  (2N7000 pin 3)
  HV_EN --- Q5 gate (pin 2)     (100k pulldown on HV_EN, already there)
                      Q5 source (pin 1) --- GND
```

- **HV_EN high:** Q5 on (Rds(on) a few ohms, negligible against 10 k), the
  divider works, Vout = 20 V.
- **HV_EN low:** Q5 off, R14 is open, pin 5 is pulled up by R13 to the rail
  (at least about 4.6 V, since D1 passes Vin to the output) which is above the
  1.25 V threshold, so the comparator inhibits the switch. This is the same
  mechanism as normal regulation ("output is high enough, skip the cycle").
- **No inverter and no extra parts**, and it **fails safe**: an open gate,
  broken wire or reset pulls HV_EN low and the converter is off.
- Caveats **[verify]**: the 2N7000's Vgs(th) can be up to about 3 V and HV_EN
  is only 3.3 V, so Rds(on) may be tens to hundreds of ohms. That shifts Vout
  by about 1-3 % (trim R14 if needed), which is fine. Pin 5's allowed input
  range must include the rail during shutdown (I recall -0.3 to +40 V; check
  the datasheet).
- **The rail does not go to 0 V when disabled.** A boost converter always has
  a DC path Vin -> L1 -> D1 -> output, so the "+20 V" rail idles at about 4.6 V.
  That is below U3's 8.4 V UVLO, so **stage 2 shuts off by itself**: no
  separate shutdown is needed for U3 (the COMP-pin 2N7000 is removed).

**Do you need the disable at all? Yes, keep it** (reasoning, 2026-10-28):

- **Idle power.** Leaving both stages running holds HV+ at the setpoint all the
  time. Rough numbers: the divider about 0.06 W at 130 V, U3 about 11 mA at 20 V
  (0.2 W), U2 and divider a few mA: roughly **0.5 W, about 100 mA from the
  5 V rail, continuously**. With the disable it is about 1 mA. That is close to
  doubling the main board's idle draw.
- **Safety.** With the disable, the board sits with **no high voltage** until
  firmware asks for it, and Cout (3.7 J at 130 V) is drained by the divider
  within a few minutes of HV_EN dropping. Always-on means a charged 130 V
  capacitor in the phone around the clock.
- **Latency is small.** The phone works at about 50-60 V (no jumper, or J9),
  which stores only about 0.3-0.5 J: stage 1 starts in tens of ms and stage 2
  reaches target in **well under 1 s** (even the 130 V profile is a few
  seconds). A coin return or collect happens when a call ends, so waiting about
  1 s is invisible. Firmware can also enable HV_EN early (at call setup or
  hang-up) and drop it after a timeout of about 10 s, which hides the charge
  time entirely.
- **If you did want it always on**, the simplest way is to tie Q5's gate to
  3.3 V (or leave Q5 out and wire R14 to GND); but I'd keep the control and
  just run it in firmware as above.
- **Things to check on the bench (not designed in):** stage 2 starts when the
  20 V rail reaches 8.4 V and then draws heavily while charging Cout. If stage
  1 (about 1 A switch limit, 1 A F1) cannot supply that, the rail may sag into
  U3's UVLO and hiccup. If so, soften U3's start (a larger Rcs for a lower
  peak current, or a soft start (Css/Rss). Rcs is already sized in stage 2 below to keep the draw near stage 1's capacity).

### Stage 2: 20 V to 52-119 V, U3 = UC3843BNG (DIP-8) + Qa = IRF630 (TO-220)

The highest setting is 119 V (J7-J9 all fitted). The sizing below uses 130 V
where it gives the worst case, as a margin.

Current-mode PWM controller, non-synchronous boost, drives an external
N-FET. The UC3843's UVLO turns on at about 8.4 V (7.8 to 9 V) and off at 7.6 V,
so it runs from the **20 V stage-1 rail** (stage 1 must start first). **Duty:**
the UCx842/UCx843 run to about 97 % (92 to 100 %); it is the UCx844/UCx845 that
are limited to 50 % by a toggle flip-flop. 20 V to 130 V needs about 85 %, so the
**UC3843 is the right part** and the UC3844/5 would not work. (An earlier version
of this note had the two backwards.)

**Component labels for our boost converter.** The UC3843A datasheet's
page-1 simplified application diagram shows the IC with a basic power stage
but no component labels. The labels below are ours, named to be
self-describing (lowercase subscripts throughout). Where a name matches a
datasheet figure (Qa, Rg, Rcs, Rt, Ct, Cf, Rz, Cz, Cvref from the
Figure 7-1 flyback example or its design figures) we kept it; everything
else is named for its function. Figure 7-1 is an off-line isolated flyback
— most of its parts (AC rectifier, transformer, opto-isolator, TL431, RCD
clamp, VCC start-up resistor) do not exist in our DC-input non-isolated
boost and are not listed here.

| Label | Description | Value | Comments |
|---|---|---|---|
| **— POWER STAGE —** | | | |
| L2 | Boost inductor (+20 V to switch node) | **220 uH, Isat >= 1 A** | Bourns RLB9012-221KL or Wurth 7447471221 |
| Qa | External power MOSFET (drain to switch node, source to Rcs) | **IRF630PBF** (TO-220, 200 V, 9 A) | Name kept from the datasheet's flyback example |
| D2 | Boost output diode (switch node to Cout) | **UF4003** (DO-41, 200 V, 1 A ultrafast) | Band (cathode) toward Cout / HV+ |
| Cout | HV output capacitor | **220 uF / 200 V** | Nichicon UCS2D221MHD; 18 x 26.5 mm, 7.5 mm leads. C1 on the schematic |
| Cin | Stage-2 input decoupling at +20 V | **2 x 22 uF / 35 V** | Close to L2; these are separate from stage 1's output caps. C14, C15 on the schematic |
| **— GATE DRIVE —** | | | |
| Rg | Gate drive resistor (OUTPUT pin 6 to Qa gate) | **100 ohm** (was 10 ohm) | Damps gate ringing, and limits the current into Dz: OUTPUT swings to about 18.5 V, so with 10 ohm Dz would carry about 0.35 A for every on-time (about 2 W average at full charge rate: Dz, Rg and U3 overheat). 100 ohm makes that 35 mA and still switches the IRF630 in about 0.3 us |
| Dz | 15 V zener gate clamp (cathode to gate, anode to Qa source) | **1N4744A** (DO-41) | Not in the datasheet's simplified diagram. OUTPUT (about 18.5 V) is just under the IRF630's 20 V Vgs maximum, so Dz is margin; it conducts every cycle, which is why Rg is 100 ohm |
| **— CURRENT SENSE —** | | | |
| Rcs | Current sense resistor (Qa source to GND) | **2 ohm, 1 W** | Ipk = 1 V / Rcs = 0.5 A; limits stage-2 input power |
| Rf | Current-sense filter resistor (Rcs to ISENSE pin 3) | **1 k** | Low-pass with Cf suppresses the leading-edge switch spike (the UC3843 has no blanking) |
| Cf | Current-sense filter cap (ISENSE pin 3 to GND) | **470 pF** | Rf x Cf = 0.47 us, well under the 15 us period. **Must sit on the ISENSE side of Rf** (pin 3 to GND): across Rcs (2 ohm) it filters nothing. The schematic currently has it on the Rcs side: move it |
| Rslope | Slope compensation resistor (RT/CT ramp to ISENSE pin 3) | **27 k, fit only if needed** | Not needed in normal running: the converter is in discontinuous mode (DCM, see Sizing), where subharmonic oscillation can't happen. It is only in continuous mode near the start of a charge, while the current limit holds the loop anyway. Keep the footprint; fit it if the switch node shows alternating long and short pulses |
| Cslope | DC blocking cap in slope-comp path (series with Rslope) | **1 nF** | Passes only the oscillator ramp, blocks the DC on RT/CT. **Its far end must go to ISENSE pin 3** (after Rf), not to the top of Rcs, where 2 ohm shorts the ramp out. The schematic currently has it on the Rcs side: move it |
| **— OSCILLATOR —** | | | |
| Rt | Timing resistor (VREF pin 8 to RT/CT pin 4) | **12 k** | fOSC = 1.72 / (Rt x Ct). R18 on the schematic |
| Ct | Timing capacitor (RT/CT pin 4 to GND) | **2.2 nF** | fOSC = 65 kHz. Never below ~1000 pF |
| **— FEEDBACK —** | | | |
| Rfbt1-3 | FB divider top (HV+ to VFB pin 2) | **3 x 100 k in series** (300 k) | Each resistor sees up to about 41 V, inside an 0603's 50-75 V rating. The divider is also Cout's main bleed (see Rbl1-3) |
| Rfbb1-4 | FB divider bottom (VFB pin 2 to GND) | **Rfbb4 15 k always fitted**; Rfbb3 100 k, Rfbb2 30 k, Rfbb1 22 k added in parallel by J9 / J8 / J7 | Vout = 2.5 x (1 + Rfbt / Rfbb), Rfbb being the parallel total; all E24 values. Table in section 5 |
| Rz | Compensation resistor (COMP pin 1 to VFB pin 2, in series with Cz) | **1 M** | Sets the mid-band gain Rz / Rfbt = 3.3, so the crossover is about 50 Hz at 60 V and full current (working under Sizing) **[verify]** |
| Cz | Compensation zero cap (in series with Rz) | **150 nF**, film or X7R | Zero at 1 / (2 pi x Rz x Cz) = 1.1 Hz, below the lowest crossover (about 5 Hz at light load), for 80-90 degrees of phase margin **[verify]** |
| Cp | High-frequency pole cap (COMP pin 1 to VFB pin 2, across Rz + Cz) | **100 pF** | Pole at 1 / (2 pi x Rz x Cp) = 1.6 kHz: keeps switching noise out of the error amplifier |
| **— IC DECOUPLING —** | | | |
| Cvcc | VCC bypass ceramic (pin 7 to GND, close to pins) | **0.1 uF** | Mandatory. A 10-22 uF electrolytic in parallel is optional (the +20 V rail already has bulk caps) |
| Cvref | VREF bypass ceramic (pin 8 to GND, close to pins) | **0.1 uF** | Mandatory per the datasheet |
| **— PROTECTION —** | | | |
| D3 | OVP TVS clamp across Cout | **1.5KE150A** (DO-201) | 128 V standoff; clamps below 200 V parts' limit |
| Rbl1-3 | Backup bleed across Cout | **3 x 1 M in series** (3 M) | The feedback divider (306-315 k) is the main bleed (τ ≈ 69 s). This chain is the independent backup if Rfbb4 opens, which stops the converter but leaves the divider no path to GND: τ = 3 M x 220 uF = 11 min, 5.6 mW at 130 V, about 43 V per resistor |
| **— OPTIONAL —** | | | |
| Css, Rss | Soft start (on COMP pin) | **omit unless needed** | Add only if stage 2 loads the 20 V rail too hard at start-up |

**U3 pin connections (UC3843BNG, DIP-8, boost topology):**

| Pin | Name | Net / connection |
|---|---|---|
| 1 | COMP | E/A output. Rz/Cz to pin 2. Pulling COMP below two diode drops would shut U3 down (Figure 7-10), but it is not needed here: U3 has no Vcc (below UVLO) whenever stage 1 is disabled |
| 2 | VFB | Divider midpoint: top Rfbt = 3 x 100 k from HV+, bottom Rfbb4 (always) plus Rfbb1-3 through J7-J9, to GND. Internal reference is **2.5 V**, so Vout = 2.5 x (1 + Rfbt/Rfbb) |
| 3 | ISENSE | Top of Rcs (Qa source), through the optional Rf/Cf filter. The cycle ends when ISENSE reaches **1 V** |
| 4 | RT/CT | Rt from pin 8, Ct to GND. fOSC = 1.72 / (Rt x Ct) = **about 65 kHz** (Rt above 5 k) |
| 5 | GROUND | GND |
| 6 | OUTPUT | Qa gate through Rg (100 ohm), clamped by Dz (15 V zener, cathode to gate, anode to Qa source). Optional Schottky from GND to OUTPUT per the datasheet |
| 7 | VCC | +20 V (stage-1 rail), Cvcc (0.1 uF ceramic) to GND. Datasheet: 12 to 28 V recommended, 30 V absolute max, about 11 to 17 mA running |
| 8 | VREF | **5 V** reference (not 2.5 V). It only feeds Rt here. Cvref **(0.1 uF ceramic to GND, mandatory)** |

**Sizing the numbers** (my arithmetic from the datasheet's equations; **[verify]** on the bench):

- **Rcs = VISENSE / Ipk = 1 V / 0.5 A = 2 ohm.** With L2 = 220 uH at 65 kHz the
  most power one cycle pair can move is about 1/2 x L x Ipk^2 x f = 1.8 W, so
  Ipk = 0.5 A caps stage 2's input at about what stage 1 can supply (about 2 W
  at 20 V). That is also what answers the start-up hiccup worry: **Rcs is the
  knob that keeps stage 2 from collapsing the 20 V rail.** Charging 3.7 J at
  about 1.5 W takes about 2.5 s, which matches the firmware's 3 s wait.
- **Frequency:** 1.72 / (12 k x 2.2 nF) = 65 kHz, inside the 50 to 100 kHz plan.
  The oscillator dead time (about 1 us) costs only about 6 % of duty, so 85 % is reachable.
- **Mode: discontinuous (DCM) once HV+ is above about 31 V.** At the 0.5 A
  peak the switch is on for t_on = L2 x Ipk / Vin = 220 uH x 0.5 / 20 = 5.5 us,
  and L2 empties in t_off = L2 x Ipk / (Vout - Vin): 2.75 us at 60 V, 1 us at
  130 V. Both sum to less than the 15.4 us period, so the current returns to
  zero every cycle. (At light load the peak is lower and it is deeper in DCM.)
  That gives no right-half-plane zero and no subharmonic oscillation, so slope
  compensation isn't needed, and the duty is only about 36 %. The 85 % figure
  above is the continuous-mode number, which this converter never runs at.
- **Loop compensation (Rz, Cz, Cp).** In DCM each cycle delivers 1/2 x L2 x Ipk^2,
  and Ipk follows COMP: Ipk = (V_COMP - 1.4 V) / (3 x Rcs), a gain of
  1 / (3 x 2 ohm) = 1/6 A per volt. Above a very low pole, at
  1 / (pi x R_load x Cout) = 0.01 Hz with only the divider as load, the power
  stage is an integrator:

  G(s) = L2 x f x Ipk / (3 x Rcs x Vout x Cout x s)

  Rz + Cz from COMP to VFB make a Type II compensator: an integrator with a zero
  at 1 / (2 pi x Rz x Cz), flat at Rz / Rfbt above it. So the crossover is

  fc = (Rz / 300 k) x L2 x f x Ipk / (2 pi x 3 x Rcs x Vout x Cout)

  | Case | Ipk | fc with Rz = 1 M |
  |---|---|---|
  | 60 V, full current (charging) | 0.5 A | about 50 Hz |
  | 60 V, divider load only | about 60 mA | about 6 Hz |
  | 130 V, full current | 0.5 A | about 22 Hz |
  | 130 V, divider load only | about 0.12 A | about 5 Hz |

  The loop gain falls with Ipk, so the zero goes below the lowest crossover:
  **Cz = 150 nF** puts it at 1.1 Hz, for 80-90 degrees of phase margin in every
  case. **Cp = 100 pF** across both adds a pole at 1.6 kHz against switching
  noise. Cout's ESR zero (about 1 kHz) is far above the crossover. A low
  crossover suits this job: Cout is large, the load is one relay pulse, and
  while charging the loop is pinned at the current limit anyway.
  **[verify] on the bench:** overshoot at the end of a charge (Cz winds up
  while the error amplifier is saturated; watch that HV+ stays below D3's 128 V
  standoff on J7), and audible chirping from skipped cycles at light load
  (harmless). A smaller Cz (47 nF, zero at 3.4 Hz) cuts overshoot at the cost
  of light-load phase margin.

- **Qa: Vishay IRF630PBF** (TO-220, 200 V, 9 A). Gate drive comes from the
  UC3843's OUTPUT (pin 6), which swings close to its VCC (about 20 V).
  **Clamp the IRF630 gate with Dz (15 V zener, DO-41)** so Vgs stays safe.
  Gate through Rg (100 ohm) to damp ringing and limit Dz's current.

  ```
  Qa gate clamp (Dz, 15 V zener):

  UC3843 pin 6 (OUTPUT) --- Rg 100R --- Qa gate (IRF630)
                                          |
                                       Dz 15V zener (band toward gate)
                                          |
                                       Qa source

  Dz clamps Vgs to 15 V. Band (cathode) faces the gate;
  anode to source. Current flows gate-to-source through
  Dz when the drive exceeds 15 V.
  ```
- **Shutdown:** none needed on U3. Disabling stage 1 (Q5, above) drops its
  rail to about 4.6 V, below U3's 8.4 V / 7.6 V UVLO, so HV_EN gates both
  stages. (This replaces the earlier COMP-pin 2N7000.)
- **L2: about 220 uH, Isat >= 1 A, shielded.** Starting value only: the
  datasheet's sizing equations decide it. Frequency about 50-100 kHz.
- **D2: UF4003** (DO-41, 200 V, 1 A ultrafast). The average current is only
  tens of mA, so 1 A is plenty.

  ```
  Stage 2 boost topology:

              L2                  D2 (UF4003)
  +20V --+-- [220uH] ---+----->|----+---> HV+ (Cout)
         |               |          |
        Cin           SW node     Cout          D3 (1.5KE150A)
       2x22uF         Qa drain    220uF           OVP clamp
       /35V              |        /200V     (see below)
         |            Qa source      |
        GND              |          GND
                        Rcs
                       (UC3843
                       ISENSE)
                         |
                        GND

  D2 anode  = switch node (L2 / Qa drain junction)
  D2 cathode = HV+ output rail (Cout positive)
  Band (cathode mark) faces the output, same as D1.
  ```
- **Cout: 220 uF / 200 V**, or 2 x 100 uF / 200 V in parallel (bulky
  radial, about 16 x 25 mm: check the pad fit). Energy at 130 V is about 3.7 J.
  For a pulse of 100 mA for 50 ms (5 mC) the droop is about 23 V
  (dV = Q / C); once the relay pull-in current is measured, resize Cout.
- **Feedback** (UC3843 E/A reference = 2.5 V): Vout = 2.5 x (1 + Rfbt / Rfbb).
  **Rfbt = 3 x 100 k in series (300 k).** Each resistor sees up to about 41 V.
  **Rfbb4 (15 k) is always fitted** from VFB to GND; each jumper adds one more
  resistor in parallel, which lowers Rfbb and raises the voltage:

  Rfbb3 = 100 k (J9), Rfbb2 = 30 k (J8), Rfbb1 = 22 k (J7). **Every
  combination of jumpers is a valid setting**, and all three together is the
  highest:

  | Jumpers fitted | Rfbb total | Vout (about) |
  |---|---|---|
  | none | 15 k (Rfbb4 alone) | **52 V**, the default |
  | J9 | 13.0 k | 60 V |
  | J8 | 10 k | 78 V |
  | J9 + J8 | 9.1 k | 85 V |
  | J7 | 8.9 k | 87 V |
  | J9 + J7 | 8.2 k | 94 V |
  | J8 + J7 | 6.9 k | 112 V |
  | J9 + J8 + J7 | 6.4 k | **119 V**, the maximum |

  Why this way: no jumper mistake can go above 119 V, below D3's 128 V
  standoff, so there is no "one jumper only" rule to get wrong. A missing,
  lifted or dirty jumper gives a lower voltage, never off or higher. An open
  Rfbb4 turns the converter off (VFB then sits at HV+), and Rbl1-3 still
  drain Cout. (The first scheme, one resistor per jumper with nothing always
  fitted, reached 215 V with two jumpers.)

  **Rfbt open or missing** would let the output run away: that is the failure
  to protect against.
- **OVP: a 150 V TVS clamp across Cout** — **1.5KE150A** (DO-201, through-hole;
  **128 V standoff**, breakdown about 143-158 V: fine with 119 V as the highest
  profile **[verify]**) so a feedback fault clamps below the 200 V parts' limit,
  at the cost of a clamp that may overheat if the fault persists.

  ```
  D3 (1.5KE150A) OVP clamp and the feedback divider across Cout:

  HV+ ---+-------+-------+-----------+
         |       |       |           |
        Cout    D3     Rfbt1-3     Rbl1-3
       220uF  1.5KE    3x 100k     3x 1M
       /200V  150A       |           |
         |    (TVS)      +-- VFB     |
         |       |       |   (pin 2) |
         |       |     Rfbb4         |
         |       |    (+ J7-J9)      |
  GND ---+-------+-------+-----------+

  D3 is a unidirectional TVS: band (cathode) toward HV+,
  anode toward GND. It clamps HV+ below about 158 V if
  the feedback loop fails. The divider drains Cout
  (about 69 s time constant) after HV_EN goes low;
  Rbl1-3 (11 min) are the backup if Rfbb4 opens.
  ```
- **Bleed: the feedback divider, plus a slow backup.** Rfbt + Rfbb is 306-315 k
  across Cout: a time constant of about 69 s with 220 uF, under 50 V about a
  minute after HV_EN drops from 119 V, and about 0.05 W at 119 V. The divider
  only drains Cout while Rfbb4 is connected, so **Rbl1-3, 3 x 1 M in series**,
  sit across Cout as an independent path: 11 min time constant on their own,
  5.6 mW at 130 V, about 43 V per resistor. Together the two give about 62 s.
  (A failed-open Rfbt runs the output up to D3, so that case is covered by the
  clamp, not the bleed.)
- **Soft rise:** the converter's soft-start plus the F1 fuse limit input
  current; **[verify]** charging 3.7 J does not pull the 5 V rail below spec.

## 6. Firing and polarity

**Superseded for wiring by [output-stage-wiring.md](output-stage-wiring.md)**,
which has the K1 pin numbers, the gate drive with Q4/Q6 and the parts to add.
The outline below is the original concept.

```
 K1 set 1:  COM1 -> J2.A (white)   NC1 -> HV+      NO1 -> RETURN
 K1 set 2:  COM2 -> J2.B (green)   NC2 -> RETURN   NO2 -> HV+

   relay off: A = HV+,    B = RETURN   (one direction)
   relay on : A = RETURN, B = HV+      (the other direction)

 RETURN = drain of Q3 (IRF630, TO-220); Q3 source -> Rsense 10 ohm -> GND
 Current path: HV+ -> K1 -> coil -> K1 -> RETURN -> Q3 -> Rsense -> GND
```

Which direction is return and which is deposit is still to confirm on the
bench (earlier: + on red / white at ground moved the lever right = return).

- **K1: DPDT signal relay, 5 V coil, contacts rated at least 220 V DC.**
  **Omron G6S-2-DC5** (through-hole): distributor listing shows 250 VAC / 220 VDC
  switching, **verify against the datasheet**. The **TQ2-5V** (110 V DC max) is
  **not** enough for the 112 and 119 V settings, fine only up to about 100 V.
- Coil of K1: +5 V through **Q2 (2N7000, TO-92)** from GP5
  (100 k pulldown on GP5), flyback diode **1N4148** (DO-35) across the coil.

  ```
  D4 (1N4148) flyback diode across K1 relay coil:

                 +5V
                  |
              K1 coil
            +----[ ]----+
            |           |
           D4          Q2 drain (2N7000)
         1N4148            |
        (band up,       Q2 source
        cathode            |
        to +5V)           GND
            |
            +--- Q2 drain

  GP5 --+-- Q2 gate
        |
      100k
        |
       GND

  D4 anode  = Q2 drain (low side of coil)
  D4 cathode = +5V (high side of coil)
  Band (cathode mark) faces +5V.
  When Q2 turns off, the coil's inductive kick forward-biases
  D4 and the energy recirculates safely instead of spiking Q2.
  ```

- **Q3: Vishay IRF630PBF** (TO-220, 200 V, 9 A). The IRF630 needs about 4-5 V
  Vgs to turn on properly — **3.3 V from the MCP23008 is not enough**. Drive
  its gate from the **+20 V stage-1 rail** through a resistor and a **12 V zener (D6)**
  (gate clamp), pulled down by **two 2N7000s** (TO-92) in a non-inverting stage
  from GP6. With HV_EN off the rail idles at only about 4.6 V (not 0 V), which
  can still partly turn Q3 on if GP6 were high, so **Q3 is held off by GP6 low**
  (10 k pulldown) and firmware never sets FIRE without HV. 100 ohm gate
  resistor on Q3.
  Current is limited by the relay's own resistor and self-interrupting switch
  (about 50 mA at 60 V measured).

  ```
  Q3 gate drive: non-inverting level shifter from GP6 (3.3 V) to +20 V

       +20V            +20V
        |                |
       4.7k (R15)      4.7k (R16)
        |                |
        +-- Q6 gate      +--- 100R --- Q3 gate (IRF630)
        |                |                 |
      Q4 drain         Q6 drain         D6 12V zener (band toward gate)
        |                |                 |
      Q4 source        Q6 source        Q3 source
        |                |                 |
       GND              GND            Rsense 10R
                                           |
  GP6 --+-- Q4 gate                       GND
        |
       10k
        |
       GND

  Two common-source 2N7000 stages, each inverts once = non-inverting:

  GP6 high -> Q4 on  -> Q4 drain low  -> Q6 gate low
           -> Q6 off -> R16 pulls high -> Q3 gate ~12 V -> Q3 ON
  GP6 low  -> Q4 off -> R15 pulls high -> Q6 gate high
           -> Q6 on  -> Q6 drain low  -> Q3 gate 0 V  -> Q3 OFF

  D6 (12 V zener): anode to Q3 source, cathode (band) to Q3 gate.
  Same orientation as the Qa gate clamp zener (Dz).
  ```
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

## 8. Voltage monitor options (2026-10-27)

**The MCP23008's A0, A1, A2 are I2C address pins, not analog inputs.** It has
no ADC; every pin is digital. Two ways to watch the high-voltage output:

**Option A: a two-bit window comparator (all through-hole, about $0.10).**
An **LM393N** (DIP-8, dual comparator; STMicro listing $0.089 at DigiKey, the
TI/onsemi/Fairchild listings were unavailable) reads a divider of the HV rail
(about 1.17 M top, 30 k bottom, so 130 V reads about 3.25 V and 60 V about
1.5 V) against two fixed thresholds from a divider off 3.3 V:
- **HV_OK** (GP3): high once the output is above about 90 % of the chosen profile;
- **HV_HIGH** (GP7): high if it is above about 110 %, an overvoltage alarm.
The outputs are open-collector, pulled up to 3.3 V. This uses the two spare
pins (GP3, GP7, which also go to J5). Firmware then knows "charged" and
"too high"; it doesn't get a number. The thresholds depend on the J7-J9
profile, so set them with a trimmer, or compute for the highest profile only.

**Option B: a real number over I2C.** A through-hole I2C ADC isn't a good
choice: **PCF8591P (DIP-16) is obsolete** at DigiKey. The practical part is an
**ADS1115** (16-bit, 4 channels, 0x48), but its package is MSOP-10, so use
a **breakout module** on a 4-pin header (VCC, GND, SDA, SCL). AIN0 reads the HV divider
(1.17 M / 33 k: 130 V reads 3.6 V, the ADS1115's 4.096 V range), AIN1 the
voltage across Rsense (fire current), AIN2 the 20 V stage. Firmware can then
check "reached target within N s", and see **the cap sag after a fire**
(the relay really fired). Cost about a few dollars for the module.

**Other I2C ADCs, DigiKey qty-1 prices on 2026-10-27** (addresses are from
memory: check each datasheet):

| Part | Bits / channels | Package | Price | Notes |
|---|---|---|---|---|
| ADS1115IDGSR | 16 / 4 | VSSOP-10, 0.5 mm | $5.28 | the original pick, 0x48 |
| ADS1015IDGSR | 12 / 4 | VSSOP-10, 0.5 mm | $3.94 | same pins and footprint as the ADS1115, 12-bit is plenty |
| **MCP3221A5T-E/OT** | 12 / 1 | SOT-23-5, 0.95 mm | **$1.91** | 18,713 in stock; easiest to hand-solder; one channel (HV only); address code A5 is 0x4D |
| MCP3021A5T-E/OT | 10 / 1 | SOT-23-5 | $1.41 | 10-bit gives about 130 mV per step on a 130 V reading |
| MCP3425A0T-E/CH | 16 / 1 | SOT-23-6 | $2.48 | one channel |
| MCP3428-E/SL | 16 / 4 | SOIC-14, 1.27 mm | $4.63 | 6,898 in stock; 4 channels in a hand-solderable package |
| ADS1110A0IDBVT | 16 / 1 | SOT-23-6 | $6.82 | dearer than the ADS1115 |

For one HV reading, a 12-bit MCP3221 gives about 32 mV per step on the 130 V
scale. Two channels (HV and fire current) need the MCP3428 or the ADS1x15.

**Either way**, the monitor only means something while HV_EN is on. If the
relay doesn't fire the voltage stays up; if it does, the cap sags by
dV = I x t / C (about 20 V for 100 mA x 50 ms into 220 uF).

## 9. Parts list (through-hole)

Links marked with a part number go to that DigiKey listing; the resistor and
small-capacitor links are DigiKey searches on the exact maker part number
(pick the cut-tape or bulk listing). Check stock and the datasheet before ordering.

| Ref | Value | Part (package) | DigiKey | Note |
|---|---|---|---|---|
| U1 | I/O expander | **MCP23008-E/P** (DIP-18) | [MCP23008-E/P](https://www.digikey.com/en/products/result?keywords=MCP23008-E/P) | address 0x20, A0-A2 to GND |
| U2 | 5 V to 20 V boost | **MC34063AP1G** (DIP-8) | [MC34063AP1G](https://www.digikey.com/en/products/result?keywords=MC34063AP1G) | 1.25 V ref |
| U3 | boost controller | **UC3843BNG** (DIP-8; onsemi's UC3843AN is obsolete, TI's UC3843AN is an equal) | [UC3843BNG](https://www.digikey.com/en/products/result?keywords=UC3843BNG) | E/A reference 2.5 V, VREF pin 5 V; runs from the +20 V rail |
| Qa | stage-2 switch, 200 V N-FET | **IRF630PBF** (TO-220) | [IRF630PBF](https://www.digikey.com/en/products/detail/vishay-siliconix/IRF630PBF/812782) | gate clamped with Dz |
| Q3 | firing FET, 200 V N-FET | **IRF630PBF** (TO-220) | [IRF630PBF](https://www.digikey.com/en/products/detail/vishay-siliconix/IRF630PBF/812782) | gate from the +20 V rail through the level shifter, clamped with D6 |
| Q2, Q4, Q5, Q6 | small N-FET | **2N7000** (TO-92) | [2N7000](https://www.digikey.com/en/products/detail/stmicroelectronics/2N7000/654416) | Q2 relay driver, Q5 HV enable, Q4 + Q6 level shifter (Q6 not placed yet) |
| K1 | DPDT relay, 5 V coil | **Omron G6S-2-DC5** (through-hole) | [G6S-2-DC5](https://www.digikey.com/en/products/detail/aratas-formerly-omron-components/G6S-2-DC5/333982) | 250 VAC / 220 VDC switching per the listing **[verify datasheet]** |
| D1 | stage-1 Schottky | **1N5819** (DO-41, 40 V, 1 A) | [1N5819](https://www.digikey.com/en/products/detail/taiwan-semiconductor-corporation/1N5819/7357079) |  |
| D2 | 200 V ultrafast | **UF4003** (DO-41, 200 V, 1 A) | [UF4003](https://www.digikey.com/en/products/detail/onsemi/UF4003/1048948) |  |
| D3 | OVP clamp | **1.5KE150A** (DO-201) | [1.5KE150A](https://www.digikey.com/en/products/detail/taiwan-semiconductor-corporation/1-5KE150A/15783456) | 128 V standoff |
| D4 | relay flyback | **1N4148** (DO-35) | [1N4148](https://www.digikey.com/en/products/result?keywords=1N4148) |  |
| Dz | Qa gate clamp, 15 V | **1N4744A** (DO-41) | [1N4744A](https://www.digikey.com/en/products/result?keywords=1N4744A) |  |
| D6 | Q3 gate clamp, 12 V | **1N4742A** (DO-41) | [1N4742A](https://www.digikey.com/en/products/result?keywords=1N4742A) |  |
| L1 | 47-100 uH, >= 2 A | **[find]** through-hole inductor |  | MC34063A's lower frequency needs a larger inductor |
| L2 | 220 uH, >= 1 A | Bourns **RLB9012-221KL** or Wurth 7447471221 | [RLB9012-221KL](https://www.digikey.com/en/products/detail/bourns-inc/RLB9012-221KL/1969608) | **[verify Isat]** |
| F1 | PTC, 1.1 A hold | Littelfuse **RXEF110** (radial disc) | [RXEF110](https://www.digikey.com/en/products/detail/littelfuse-inc/RXEF110/1045810) | section 5: 500 mA is too small. **Not on the schematic yet** |
| C1 (Cout) | 220 uF / 200 V | Nichicon **UCS2D221MHD** (radial, 18 x 26.5 mm, 7.5 mm leads) | [UCS2D221MHD](https://www.digikey.com/en/products/detail/nichicon/UCS2D221MHD/4991877) |  |
| C5, C7, C9, C14, C15 | 22 uF / 35 V electrolytic | Panasonic **EEU-FR1V220** (radial 5 x 11 mm, 2 mm leads) | [EEU-FR1V220](https://www.digikey.com/en/products/result?keywords=EEU-FR1V220) | C5 +5 V input, C7/C9 stage-1 output, C14/C15 stage-2 input. One part for all five |
| C2, C3, C4, C6, Cvcc, Cvref | 100 nF / 50 V, X7R | TDK **FG28X7R1H104KNT06** (radial, 5 mm leads) | [FG28X7R1H104KNT06](https://www.digikey.com/en/products/result?keywords=FG28X7R1H104KNT06) | decoupling and coin-line filters: X7R is fine |
| Ct | 2.2 nF / 50 V, **C0G (NP0)** | TDK **FG28C0G1H222JNT06** (radial, 5 mm leads) | [FG28C0G1H222JNT06](https://www.digikey.com/en/products/result?keywords=FG28C0G1H222JNT06) | **Must be C0G/NP0:** it sets U3's 65 kHz clock. X7R loses 10-15 % with temperature and more with voltage, so the frequency (and Ipk per cycle) would drift |
| C8 | 1 nF / 50 V, **C0G (NP0)** | TDK **FG28C0G1H102JNT06** (radial, 5 mm leads) | [FG28C0G1H102JNT06](https://www.digikey.com/en/products/result?keywords=FG28C0G1H102JNT06) | **C0G/NP0:** U2's timing cap, same reason as Ct |
| Cf | 470 pF / 50 V, **C0G (NP0)** | TDK **FG28C0G1H471JNT06** (radial, 5 mm leads) | [FG28C0G1H471JNT06](https://www.digikey.com/en/products/detail/tdk-corporation/FG28C0G1H471JNT06/5812105) | **Must be C0G/NP0:** with Rf it sets the current-sense filter; a drifting value moves the current limit timing. From ISENSE (pin 3) to GND |
| Cp | 100 pF / 50 V, **C0G (NP0)** | TDK **FG28C0G1H101JNT06** (radial, 5 mm leads) | [FG28C0G1H101JNT06](https://www.digikey.com/en/products/result?keywords=FG28C0G1H101JNT06) | **Must be C0G/NP0:** small values only come in C0G anyway, and it sets the compensation pole |
| Cslope | 1 nF / 50 V | TDK **FG28C0G1H102JNT06** (radial, 5 mm leads) | [FG28C0G1H102JNT06](https://www.digikey.com/en/products/result?keywords=FG28C0G1H102JNT06) | DC block only, any dielectric works; same part as C8 to save a line |
| Cz | 150 nF / 63 V film | WIMA **MKS2C031501A00JSSD** (box, 5 mm leads) | [MKS2C031501A00JSSD](https://www.digikey.com/en/products/result?keywords=MKS2C031501A00JSSD) | film (or X7R); sets the compensation zero |
| R1, R2 | 100 k | 1/4 W 1 % metal film (Yageo MFR-25) | [MFR-25FBF52-100K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-100K) | HV_EN and POL pulldowns |
| R3 | 10 k | 1/4 W 1 % metal film | [MFR-25FBF52-10K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-10K) | FIRE pulldown |
| R4, R5, R6 | 10 k | 1/4 W 1 % metal film | [MFR-25FBF52-10K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-10K) | coin-line pull-ups to 3.3 V |
| R7, R8, R9 | 1 k | 1/4 W 1 % metal film | [MFR-25FBF52-1K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-1K) | coin-line series resistors |
| R10 | 10 k | 1/4 W 1 % metal film | [MFR-25FBF52-10K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-10K) | MCP23008 reset pull-up |
| R11 | 0.33 ohm, 0.5 W | TE **LR1LJR33** (axial, 5 %) | [LR1LJR33](https://www.digikey.com/en/products/detail/te-connectivity-passive-product/LR1LJR33/2381302) | U2 current sense (RSC) |
| R12 | 68 ohm | 1/4 W 1 % metal film | [MFR-25FBF52-68R](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-68R) | U2 pin 8 drive resistor |
| R13 | 150 k | 1/4 W 1 % metal film | [MFR-25FBF52-150K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-150K) | stage-1 divider top |
| R14 | 10 k | 1/4 W 1 % metal film | [MFR-25FBF52-10K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-10K) | stage-1 divider bottom |
| R18 (Rt) | 12 k | 1/4 W 1 % metal film | [MFR-25FBF52-12K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-12K) | U3 oscillator |
| Rcs | 2 ohm, 1 W | Yageo **FMP100FTF52-2R** (axial) | [FMP100FTF52-2R](https://www.digikey.com/en/products/detail/yageo/FMP100FTF52-2R/9106897) | stage-2 current sense: Ipk = 1 V / 2 ohm |
| Rf | 1 k | 1/4 W 1 % metal film | [MFR-25FBF52-1K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-1K) | current-sense filter |
| Rg | 100 ohm | 1/4 W 1 % metal film | [MFR-25FBF52-100R](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-100R) | Qa gate |
| Rslope | 27 k | 1/4 W 1 % metal film | [MFR-25FBF52-27K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-27K) | **fit only if needed** (section 5) |
| Rz | 1 M | 1/4 W 1 % metal film | [MFR-25FBF52-1M](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-1M) | compensation |
| Rfbt1-3 | 100 k | 1/4 W 1 % metal film (250 V rated) | [MFR-25FBF52-100K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-100K) | feedback divider top, and Cout's bleed |
| Rfbb4 | 15 k | 1/4 W 1 % metal film | [MFR-25FBF52-15K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-15K) | **always fitted**: 52 V with no jumper |
| Rfbb3, Rfbb2, Rfbb1 | 100 k / 30 k / 22 k | 1/4 W 1 % metal film | [MFR-25FBF52-100K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-100K), [MFR-25FBF52-30K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-30K), [MFR-25FBF52-22K](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-22K) | added in parallel by J9 / J8 / J7; any combination, 52-119 V (section 5) |
| Rbl1-3 | 1 M | 1/4 W 1 % metal film (250 V rated) | [MFR-25FBF52-1M](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-1M) | backup bleed across Cout, 3 in series. **Not placed yet** |
| R15, R16 | 4.7 k | 1/4 W 1 % metal film | [MFR-25FBF52-4K7](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-4K7) | level-shifter pull-ups. **Not placed yet** |
| Rgq3 | 100 ohm | 1/4 W 1 % metal film | [MFR-25FBF52-100R](https://www.digikey.com/en/products/result?keywords=MFR-25FBF52-100R) | Q3 gate. **Not placed yet** |
| Rsense | 10 ohm, 0.5 W | metal film, 0.5 W (e.g. Yageo MFR-50) | [MFR-50FBF52-10R](https://www.digikey.com/en/products/result?keywords=MFR-50FBF52-10R) | firing current sense. **Not placed yet** |
| J1 | 5-pin header, 2.54 mm | generic 1 x 5 |  | mates with v6 J6 |
| J2 | 3-pin KK 396 header | **Molex 0026604030** | [0026604030](https://www.digikey.com/en/products/result?keywords=0026604030) | mating plug 09-50-3031 + terminals **[verify]** |
| J3 | 4-pin KK 396 header | **Molex 0026604040** | [0026604040](https://www.digikey.com/en/products/result?keywords=0026604040) | mating plug 09-50-3041 + terminals **[verify]** |
| J7, J8, J9 | 2-pin header + jumper, 2.54 mm | generic |  | voltage select; any combination |

## 10. Layout notes

- Keep the **HV zone** (Cout, D2, Qa, L2, K1 contacts, Q3, J2, divider
  resistors) together, away from J1 and the logic. **Spacing at least 1.0 mm** for anything that
  can reach 130 V (IPC-2221 external bare copper is about 0.6 mm up to 150 V; use
  more); no logic traces under the HV parts, solder mask is not insulation.
- A **slot or wide gap** between the relay's coil and contact sides.
- Label the HV zone in silkscreen. The 119 V maximum is just under the 120 V DC extra-low-voltage
  line; the Cout cap stays charged until the divider drains it (about a minute), so label it.
- Short, wide switching loop for Qa/L2/D2 and the input capacitor.

## 11. Still to settle

1. **Identify the coin sensor wires** (supply, ground, 3 outputs) and logic levels.
2. **Which phone-side voltage is needed:** the settings above 60 V are margin for other
   phones; this phone works at about 50-60 V. No jumper (52 V) is the default; J9 gives 60 V.
3. **Datasheet checks** for the **[verify]** items, above all U2 MC34063A
   (timing cap, boost equations), U3 UC3843A (Rcs, Rt/Ct, Rfbt/Rfbb values) and
   K1 (contact rating).
4. **Measure pull-in charge** (current x time at the operating voltage) to size Cout.
5. Firmware: a new MCP23008 driver, coin events, and a coin-relay
   output (return/collect) with a safety timeout. **None of it exists yet.**
6. **Schematic (`Payphone_v1/Payphone_v1.kicad_sch`) against this doc.** The
   power section is drawn; these differ:
   - **Qa and Q3 are swapped:** the stage-2 switch (Dz, Rg, Rcs, D2) is drawn as
     **Q3**, and **Qa** is the unwired one. Swap the two references so Qa is the
     boost switch and Q3 the firing FET, as here.
   - **Cf and Cslope** connect to the top of Rcs; both belong on **ISENSE pin 3**
     (the far side of Rf). See the stage-2 table.
   - **Rg** is 10 ohm; make it **100 ohm**.
   - **The firing stage isn't wired yet:** K1, Q2 and D4 (relay coil driver and
     flyback), Q3 (firing FET, now drawn as Qa), D6 (its 12 V gate zener) and Q4
     (level shifter) are placed but unconnected, and the level shifter's second
     2N7000 (**Q6**), R15/R16 (4.7 k pull-ups), Q3's 100 ohm gate resistor and
     Rsense (10 ohm) aren't placed yet. J2 (to the relay) is also unconnected.
     All are still needed.
   - **Voltage select:** wire **Rfbb4 (15 k) straight from VFB to GND** and
     delete J10; change Rfbb3 to **100 k**, Rfbb2 to **30 k** and Rfbb1 to
     **22 k** (section 5's table). Add **Rbl1-3** (3 x 1 M in series) from HV+
     to GND, beside Cout.
   - **RV1** (10 k trimmer) isn't part of this design: J7-J9 set the voltage.
     Delete it. (A trimmer in the HV feedback path is also a hazard: a wiper
     that lifts would let the output run up to D3.)
   - **F1** (PTC, about 1 A hold) and **JP1** (sensor supply select) aren't on
     the schematic yet.
   - R12 is 68 ohm (82 ohm here): both work.
   - The resistors use 0603 footprints; the BOM here assumes through-hole.
     Fine either way, but pick one.
7. A PCB from this once the above are settled.

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
Cout is about 16 x 25 mm and sets the size)

```
 +----------------------------------------------------------------+
 | LOGIC ZONE (left)          | POWER (middle)  | HV ZONE (right) |
 |                            |                 |                 |
 |  J1 cable (5V,GND,SDA,SCL) |  U2 + L1 + D1   |  U3 + Qa + L2   |
 |  LDO 3.3 V                 |  (5 V -> 20 V)  |  D2, Cout,      |
 |  U1 MCP23008 (DIP-18)      |                 |  divider, TVS   |
 |  coin inputs -> J3 (edge)  |                 |  K1 relay, Q3   |
 |  J5 spare, J7-J9 volts    |                 |  J2 (to relay)  |
 +----------------------------------------------------------------+
   J1 and J3 on the left edge; J2 on the right edge, near the relay
```

### Layout rules

- **Net classes in KiCad:** an **`HV`** class (everything from Cout and D2 to
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
- **Divider resistors** (Rfbt, 3 x 100 k chain) in a straight line, evenly spaced;
  next to Cout.
- **Relay K1:** contacts on the HV side, coil on the logic side, with **at
  least 2 mm or a slot** between them if the pins allow. **Q3 and Rsense**
  next to it; J2 on the same edge keeps the coil wires short.
- **Mounting holes** (M3) at the corners, not near HV. **Silkscreen** an HV
  warning, the J7-J9 voltage table and "capacitor stays charged".
- **Test points:** 20 V, 3.3 V, HV (a probe pad with a series resistor), GND.
- **Assembly:** Cout, K1, the Molex KK headers and likely L2 are through-hole
  or large, so expect to hand-solder them (mark them "do not place" for
  JLCPCB, as on the main board).
- **DRC:** set the net classes first, run DRC with the 1.0 mm rule, then check
  clearances by hand around Cout, D2 and J2.

## 13. DigiKey parts and pricing (2026-10-27)

Through-hole is now the default throughout (sections 5, 6, 9). Design notes
for the UC3843A UVLO, gate clamping and the Q3 level-shifter are in sections
5 and 6 above.

- **Does a 100 kHz I2C bus affect the ES8388's audio?** No. Audio goes over I2S
  (a separate set of pins). I2C only writes the codec's registers (volume,
  mute, routing). The firmware already runs the codec at 100 kHz
  (`es8388.c`: `.scl_speed_hz = 100000`), so nothing changes.

**DigiKey parts (looked up 2026-10-27; prices qty 1, stock as listed then)**

| Ref | Part | Maker / part number | DigiKey | Price | Notes |
|---|---|---|---|---|---|
| U1 | MCP23008 I/O expander, DIP-18 | Microchip **MCP23008-E/P** | MCP23008-E/P-ND | $1.54 (qty 100 $1.24) | 4,996 in stock, active |
| U2 | stage 1 controller, DIP-8 | onsemi **MC34063AP1G** | MC34063AP1GOS-ND | $1.98 | 6,945 in stock. Cheaper: KA34063A, $0.40 |
| U3 | stage 2 controller, DIP-8 | onsemi **UC3843BNG** | UC3843BNGOS-ND | $1.87 | 753 in stock, active (2026-10-30). The onsemi UC3843AN is obsolete. TI's UC3843AN ($1.79, 6,770 in stock) is the same chip. **Not the UC3842BNG**: same pins, but it starts at about 16 V and stops at 10 V, too close to the 20 V rail |
| Qa, Q3 | 200 V N-FET, TO-220 | Vishay **IRF630PBF** ([link](https://www.digikey.com/en/products/detail/vishay-siliconix/IRF630PBF/812782)) | (see link) | $2.43 each | 200 V, 9 A from memory: **verify**. Needs a heatsink only if it ever runs continuous |
| Q2, Q4, Q5, Q6 | small N-FET, TO-92 | **2N7000** (e.g. [STMicro](https://www.digikey.com/en/products/detail/stmicroelectronics/2N7000/654416)) | (see link) | $0.06-$0.66 | relay driver, HV enable and the level shifter |
| K1 | DPDT relay, 5 V coil, through-hole | Omron (Aratas) **G6S-2-DC5** ([link](https://www.digikey.com/en/products/detail/aratas-formerly-omron-components/G6S-2-DC5/333982)) | (see link) | $4.52 | **250 VAC / 220 VDC switching** per the listing; 11,369 in stock |
| D1 | stage-1 Schottky | **1N5819** (DO-41, 40 V, 1 A), e.g. [TSC](https://www.digikey.com/en/products/detail/taiwan-semiconductor-corporation/1N5819/7357079) | (see link) | $0.32 | 1 A average is plenty here |
| D2 | 200 V ultrafast | **UF4003** (DO-41, 200 V, 1 A), e.g. [onsemi](https://www.digikey.com/en/products/detail/onsemi/UF4003/1048948) | (see link) | $0.64 | cheaper makers $0.02-$0.20: check reverse recovery on the datasheet |
| D3 | 150 V clamp | **1.5KE150A** (DO-201), e.g. [TSC](https://www.digikey.com/en/products/detail/taiwan-semiconductor-corporation/1-5KE150A/15783456) | (see link) | $0.19 | **standoff is 128 V**, not 150 V: fine with 119 V as the highest setting. 1.5KE170A (145 V) if you want more headroom, with a higher clamp |
| Cout | 220 uF / 200 V radial | Nichicon **UCS2D221MHD** ([link](https://www.digikey.com/en/products/detail/nichicon/UCS2D221MHD/4991877)) | (see link) | $3.08 | 18 mm dia x 26.5 mm, 7.5 mm lead spacing, 15,508 in stock. **Bigger than the 16 x 25 mm I assumed** |
| L2 | 220 uH, 1 A | Bourns **RLB9012-221KL** ([link](https://www.digikey.com/en/products/detail/bourns-inc/RLB9012-221KL/1969608)) | (see link) | $0.66 | 650 mohm DCR, Isat not listed: **check Isat against the controller's peak current**; Wurth 7447471221 ($2.49, 1.3 A) is the sturdier choice |
| L1 | 47-100 uH, 2 A+ | not found | | | **search DigiKey** for a through-hole 47-100 uH inductor with Isat >= 2 A (my search returned nothing) |
| J2 | 3-pin KK 396 header | Molex **0026604030** | WM4621-ND | $0.35 (10: $0.30) | 26,865 in stock |
| J3 | 4-pin KK 396 header | Molex **0026604040** | WM4622-ND | $0.44 (10: $0.38) | 57,598 in stock |
| J1 | 5-pin header, 2.54 mm | generic 1 x 5 | | | mates with the v6 J6 |
| Q6 | gate-drive PNP, TO-92 | **2N3906** | | | NEW for the firing switch (output-stage-wiring.md, block C) |
| R19-R24 | 1k, 1k, 4.7k, 10k, 1k, 10k | 1/4 W axial | | | NEW, output stage |
| R25 | 10 ohm, 1/2 W | axial | | | NEW, fire current sense |
| (debug) | ADS1115 ADC module | **AliExpress, about $1.90** ([listing](https://www.aliexpress.us/item/2251832488241448.html)) | | | optional, on a 1 x 10 header; bring-up boards only |

The listed parts come to about **$19.50** at qty 1, with a pair of
IRF630s for Qa and Q3 ($4.86) the biggest single item after the relay ($4.52), the
capacitor ($3.08) and the DIP chips. Add roughly **$5-$8** for the passives
and the unlisted parts (L1, header, diodes, zeners, trimmer): call it **$25-$28
a board** at DigiKey qty-1 prices, before the bare PCB. Prices from page listings;
quantity breaks will drop it.

**Small parts count (all through-hole, 1/4 W metal film and radial or
ceramic disc)**, rough, to finalize with the schematic: about **25
resistors** (divider chain, pulldowns, pull-ups, gate and base resistors,
current sense), **12 capacitors** (decoupling, timing, bulk), **6-8 diodes and
zeners**, plus the J7-J9 jumpers. Not cheap in hand-soldering time (about an
hour for a few boards) but cheap in parts.

**Do these before ordering**
1. Check each datasheet for the stage-1 and stage-2 equations: MC34063A boost
   at 5 V in and the UC3843A boost at 20 V to 130 V (see **[verify]** notes
   in sections 5 and 6).
2. Check the IRF630 (Qa) gate drive, Isat on L2, and the relay's contact rating at
   the datasheet, not the listing.

## 14. KiCad symbols and footprints (2026-10-27)

All from KiCad's stock libraries except three symbols I flattened and added to
the `Custom` library (`~/Documents/KiCad/Custom`: `UC3843AN`, `IRF630`,
`UF4003`; listed in its README). Footprint names are the stock ones, so they
resolve in KiCad as long as the standard libraries are loaded.

| Part | Symbol | Footprint |
|---|---|---|
| U1 MCP23008-E/P | `Interface_Expansion:MCP23008-xP` | `Package_DIP:DIP-18_W7.62mm` |
| U2 MC34063AP1G | `Regulator_Switching:MC34063AP` | `Package_DIP:DIP-8_W7.62mm` |
| U3 UC3843BNG | **`Custom:UC3843AN`** (any UC3843 DIP-8; stock name: `Regulator_Controller:UC3843_DIP8`) | `Package_DIP:DIP-8_W7.62mm` |
| Qa, Q3 IRF630 | **`Custom:IRF630`** | `Package_TO_SOT_THT:TO-220-3_Vertical` |
| Q2, Q4, Q5, Q6 2N7000 | `Transistor_FET:2N7000` | `Package_TO_SOT_THT:TO-92_Inline` (check pin order against the part) |
| K1 G6S-2-DC5 | `Relay:G6S-2` | `Relay_THT:Relay_DPDT_Omron_G6S-2` |
| D1 1N5819 | `Diode:1N5819` | `Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal` |
| D2 UF4003 | **`Custom:UF4003`** | `Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal` |
| D3 1.5KE150A | `Diode:1.5KExxA` | `Diode_THT:D_DO-201AD_P15.24mm_Horizontal` |
| D4 relay flyback 1N4148 | `Diode:1N4148` | `Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal` |
| zeners (gate clamps) | `Device:D_Zener` | `Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal` |
| **Cout 220 uF / 200 V** (Nichicon UCS2D221MHD, 18 x 26.5 mm, 7.5 mm leads) | `Device:C_Polarized` | **`Capacitor_THT:CP_Radial_D18.0mm_P7.50mm`** |
| other electrolytics (100 uF, 470 uF) | `Device:C_Polarized` | pick by the part's diameter and lead spacing: `CP_Radial_D6.3mm_P2.50mm`, `D8.0mm_P3.50mm`, `D10.0mm_P5.00mm` |
| 100 nF and small ceramics | `Device:C` | `Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm` |
| resistors, 1/4 W | `Device:R` | `Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal` |
| Rsense (fire) (0.5 to 1 W) | `Device:R` | `Resistor_THT:R_Axial_DIN0309_L9.0mm_D3.2mm_P12.70mm_Horizontal` |
| trimmer (fine voltage) | `Device:R_Potentiometer_Trim` | `Potentiometer_THT:Potentiometer_Bourns_3296W_Vertical` |
| L1, L2 | `Device:L` | `Inductor_THT:L_Radial_D10.0mm_P5.00mm_Fastron_07M` is the nearest stock footprint for the Bourns RLB9012 (9 mm): **check the lead spacing against the datasheet** |
| J1 5-pin I2C | `Connector:Conn_01x05_Pin` | `Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical` |
| J2 3-pin KK 396 (0026604030) | `Connector:Conn_01x03_Pin` | **`Connector_Molex:Molex_KK-396_A-41791-0003_1x03_P3.96mm_Vertical`** (its description says "new part number 26-60-4030") |
| J3 4-pin KK 396 (0026604040) | `Connector:Conn_01x04_Pin` | **`Connector_Molex:Molex_KK-396_A-41791-0004_1x04_P3.96mm_Vertical`** (description: "26-60-4040") |

Molex's own CAD (symbol, footprint, 3D model for any part number) is on the
part's Molex page and on DigiKey's part page, if you want the vendor version.

**Hand-soldering surface-mount** (SOT-23 and larger is OK for you): a smaller
board than the all-DIP one is possible. Candidates: **MT3608/SX1308
(SOT-23-6)** for stage 1 instead of the MC34063 and its timing parts, **2N7002
(SOT-23)** for the small switches, **ES2D (SMA)** for D2, **STN4NF20L (SOT-223)**
for Q3 (watch its 3 V gate threshold), and **UC3843 in SOIC-8** (1.27 mm pitch)
instead of the DIP. **Not** the TPS40210 (MSOP-10, 0.5 mm pitch); the
UC3843 route avoids it. 0603 passives are hand-solderable with a fine tip,
0805 is easier. The high-voltage parts (Cout, IRF630, relay, Molex headers)
stay through-hole either way.

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
