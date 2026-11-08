[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md)

# Board reference: payphone coin board

> **Sent to fab, not built or tested yet.** The design and fab files are published
> ([KiCad project](https://github.com/sdylewski/Analog-Redial/tree/main/PayPhone/Payphone_coin_board_v1)),
> but no coin board has been built, and its firmware has only run against simulated chips.
> Values and timings on this page come from datasheets and arithmetic, not measurements, and
> may change after the first build.

> ⚠️ **High voltage.** This board makes up to **119 V DC** and stores it in a 220 µF capacitor
> (C1), enough to hurt. The HV LED (D7) lights above about 24 V. After the firmware turns the
> supply off, C1 takes up to about 40 seconds to bleed down to a safe level: wait until D7 is
> dark before touching the board or the relay wires.

The coin board connects a payphone's coin mechanism to Analog Redial. It does three jobs:

1. **Fires the coin relay** in either direction, to collect the coins in the hopper or return
   them, from a high voltage made on the board (52-119 V for the HD550002C relay, about 19 V
   for 12-24 V relays).
2. **Reads the coin switches:** 5, 10 and 25 cents, plus a fourth input for another coin or a
   coin acceptor.
3. **Talks to the main board over I²C**, on a 5-wire cable. The high voltage never leaves the
   coin board.

It plugs into v6's **J11** I²C accessory header. It should also work on any board or
microcontroller with an I²C bus, 5 V and a 3.0-5.5 V logic supply.

## 1. Overview

```
 Analog Redial (v6 J11)                COIN BOARD
 +5V  --- J6.5 ---> [stage 1: 5 V -> 20 V] ---> [stage 2: 20 V -> HV] ---> C1 (HV, 52-119 V)
 VIO  --- J6.4               ^ EN_HV                  ^ J14, J7-J9                 |
 SDA  --- J6.3 --+           |                                                     |
 SCL  --- J6.2 --+--> U1 TCA9534A (0x39): EN_HV, POL --> K1 polarity relay <--------+
 GND  --- J6.1   |                        FIRE --> Q3 firing switch --> R26 --> GND
                 |                        coin 4, quarter, dime, nickel <-- J3 / J12
                 +--> U4 ADS1115 (0x48): HV, 20 V rail, firing current, spare input (J13)

 K1 --> J2 / J11 --> the payphone's coin relay
```

| Part | Does |
|---|---|
| **U1** TCA9534A I/O expander, I²C 0x39 | reads the four coin inputs; drives EN_HV (supply on), POL (direction) and FIRE |
| **U2** MC34063, stage 1 | boosts 5 V to a 20 V rail, only while EN_HV is high |
| **U3** UC3843 + Q6, stage 2 | boosts 20 V to the high voltage on C1; J7-J9 set the voltage |
| **K1** DPDT relay | picks which coin-relay wire gets HV: collect or return |
| **Q3** with R26 | the firing switch: lets current through the coin relay for the length of the pulse, and R26 measures it |
| **U4** ADS1115 module | measures HV, the 20 V rail and the firing current, so the firmware knows the relay fired |
| **D8-D13, D15** | small green LEDs: one per coin input (blinks with a coin), and one each for FIRE, POL and EN_HV |
| **D7** | red HV LED, lit above about 24 V |

**Safe at power-up.** Pull-downs hold EN_HV, POL and FIRE low until the firmware drives them,
so nothing fires and there is no high voltage until the firmware asks for it. The supply runs
only for a collect or return, then turns off again.

## 2. Connectors

The payphone has two separate cables for this board: a 4-wire one from the coin switches and a
3-wire one from the coin relay.

| Ref | Part | Pins |
|---|---|---|
| **J6** | 5-pin header, 2.54 mm | **1 GND, 2 SCL, 3 SDA, 4 VIO, 5 +5 V**: the same pin numbers as v6's J11, so the cable is wired pin 1 to pin 1 |
| **J1** | JST XH 5-pin | the same five signals, in parallel with J6: fit whichever suits your cable |
| **J2** | Molex KK 396, 3-pin | **coin relay cable** (HD550002C): 1 green, 2 red (not connected), 3 white |
| **J11** | 3-pin screw terminal, 5.08 mm | the coin relay instead of J2: 1 coil A, 2 common (two-coil relays only, see J10), 3 coil B |
| **J3** | Molex KK 396, 4-pin | **coin switch cable**: 1 blue nickel, 2 red dime, 3 white quarter, 4 green common |
| **J12** | 5-pin screw terminal, 2.54 mm | coin inputs instead of J3: 1 GND, 2 coin 4, 3 quarter, 4 dime, 5 nickel |
| **J13** | 3-pin header | spare analog input: 1 VIO, 2 input (U4 AIN3), 3 GND |

**Jumpers:**

| Ref | What it does |
|---|---|
| **J14** | **HV boost.** No shunt (the default): stage 2 can't run and HV stays at about **19 V** whatever the firmware does, which suits 12-24 V coils. Fit it for the HD550002C and other high-voltage relays; J7-J9 then set the voltage |
| **J7-J9** | with J14 fitted, set the high voltage (table below) |
| **J10** | two-coil relays only: ties J11 pin 2 to HV, so the common wire is positive and K1 picks the coil |
| **JP1** | solder jumper, open. Bridge it only for a host that has no logic supply to give, to run U1 and U4 from 5 V |

| Shunts fitted | HV (about) |
|---|---|
| none (no J14) | **19 V**, stage 2 off |
| J14 | **52 V** |
| J14 + J9 | 60 V |
| J14 + J8 | 78 V |
| J14 + J8 + J9 | 85 V |
| J14 + J7 | 87 V |
| J14 + J7 + J9 | 94 V |
| J14 + J7 + J8 | 112 V |
| J14 + J7 + J8 + J9 | **119 V**, the maximum |

Every combination is valid and the same table is printed on the board. A missing or loose
shunt gives a lower voltage, never a higher one. The GTE / Quadrum phone this was designed
around needs about 50-60 V, so start with J14 alone (52 V).

## 3. The I²C bus and VIO

| Device | Address |
|---|---|
| ES8388 codec (main board) | 0x10 |
| TCA9534A, line 2 (main board v6) | 0x38 |
| **U1** TCA9534A (coin board) | **0x39** |
| **U4** ADS1115 (coin board) | **0x48** |

The main board has the I²C pull-ups, so the coin board's own (R42, R43) aren't fitted.

**VIO** is the main board's logic voltage, 3.3 V on Analog Redial. U1 and U4 run from it, so
they read and drive I²C at the host's own level, 3.3 V or 5 V, with nothing to set. The 5 V pin
powers only the converters and K1. VIO must be 3.0-5.5 V: below that the supply's switches and
the LEDs won't turn on.

## 4. The high-voltage supply

**Stage 1 (U2, MC34063)** boosts 5 V to 20 V. It has no enable pin, so EN_HV switches the
bottom of its feedback divider (Q1): with EN_HV low the converter stops, and the rail idles at
about 4.6 V (a boost converter always passes its input through).

**Stage 2 (U3, UC3843, with Q6 and L2)** boosts 20 V to HV on C1 (220 µF, 200 V). It runs from
the 20 V rail through J14, so it stops by itself when stage 1 is off. It charges C1 at about
2 W: a fraction of a second to 60 V, about a second to 119 V.

**Protection:**

- **D3** (SMBJ130A) clamps HV if the feedback loop fails, below the 200 V parts' limit.
- **Bleed resistors** across C1 drain it after EN_HV drops: 119 V falls below 24 V (D7 goes out)
  in about 33 s, and 60 V falls below 20 V in about 32 s.
- **F1**, a 1.1 A resettable fuse, on the 5 V input.

**Supply current:** charging C1 draws about 1 A peaks from the 5 V input for up to a second.
Idle, the board draws a few mA.

## 5. Firing the relay

A collect or return runs this sequence:

1. Set **POL** (K1's direction: which coin-relay wire gets HV).
2. Raise **EN_HV** and wait for HV, which U4 reads.
3. Pulse **FIRE**: Q3 lets current through the coin relay for the pulse length (100 ms by
   default).
4. Drop FIRE, then EN_HV.

K1 only switches while no current flows, so its contacts never break the coil current. U4
measures the firing current across R26, and the HV drop on C1, so the firmware can tell
whether the relay fired. It drops FIRE early if the current looks like a short. **D14**
catches the coil's kick when Q3 turns off.

| POL | Coil A (J2/J11 pin 1, green) | Coil B (J2/J11 pin 3, white) |
|---|---|---|
| low (at power-up) | HV | return (through Q3) |
| high | return (through Q3) | HV |

On the HD550002C, green positive returns the coins and white positive collects them, the
opposite of the firmware's default, so tick **"Collect and return are the wrong way round"** on
the Coin board card. Do the same for any relay that moves the wrong way, rather than rewiring.

**Two-coil relays** (separate collect and return coils on a common wire): wire them to J11
with the common on pin 2, and fit J10. The common is then always HV, and K1 picks which coil
fires.

## 6. Coin inputs

- The phone's coin switches are passive: green is the common, and it closes briefly to white,
  red or blue for a quarter, dime or nickel. The board ties the common to GND and pulls each
  line up to VIO. A closure reads low.
- Each line has a 1 ms RC filter and its own LED; the firmware polls every few ms and
  debounces.
- **Coin 4** (J12 pin 2) is wired the same, for a 50¢ or $1 switch, a coin-present switch or a
  coin acceptor's pulse output. Set what it's worth on the Coin board card (0 means log it
  only).
- A Schottky diode (D16-D19) in each input lets the inputs take a switch to GND, an
  open-collector output or a logic output of **any voltage up to 40 V**, all active low. An
  active-high output needs its own inverter, and the inputs aren't protected against a
  negative voltage.

## 7. The cable to the main board

The cable carries only 5 V, VIO, GND, SDA and SCL. The only wires with high voltage are the
short ones from J2 or J11 to the relay.

- **Up to about 1 m** works at the firmware's 100 kHz I²C speed.
- **Twist SDA and SCL each with a ground wire,** or use shielded pairs, so the cable doesn't
  pick up the converters' switching noise.
- **Use 24 AWG or thicker** for 5 V and GND: charging C1 draws about 1 A peaks.
- If the main board's audio codec loses its settings when the cable is touched, the cable is
  loading the bus: an I²C buffer (TCA4307 or PCA9517 class) at the main-board end fixes that.

## 8. Ordering and building

JLCPCB fabricates the board and places the surface-mount parts from the files in
[`jlcpcb/production_files/`](https://github.com/sdylewski/Analog-Redial/tree/main/PayPhone/Payphone_coin_board_v1/jlcpcb/production_files)
(gerbers, BOM and placement). You solder the rest:

- **Through-hole:** C1, K1, the connectors (J1/J6, J2/J11, J3/J12, J13), the jumper headers
  and shunts (J7-J10, J14), and U4: a ready-made ADS1115 module on a 10-pin header (a socket
  makes it easy to swap).
- **Surface-mount parts left off to save JLCPCB's Extended-part fees:** C5, C7, C14, C20
  (100 µF 50 V), U3 (UC3843 in SOIC-8), F1 and D3. All are large and easy to hand-solder.

Fit **J2 or J11** for the relay and **J3 or J12** for the coin switches, not both. Leave
**J14 off** for the first power-up.

Estimated cost for 5 boards: about **$40 a board**, delivered to the US with the hand-solder
parts. An estimate, not a quote.

## 9. Firmware and first power-up

The firmware is on the **Payphone** tab (Settings → Payphone, marked Experimental): turn on
the Payphone features, then on the **Coin board** card tick "A coin board is connected" and
choose its line. The card then shows the board live (U1 and U4 answering, each coin switch,
the 20 V rail, HV and the coil current), and has test buttons for **Collect**, **Return** and
**HV on for 10 s** (charges without firing, to read the voltage). The console has the same as
`coinboard [collect|return|hv]`.

Suggested first power-up, one step at a time:

1. No relay, J14 off. Connect the cable and tick "A coin board is connected": the card should
   show U1 and U4 answering and the 20 V rail at about 4.6 V.
2. Close each coin switch by hand: the card shows it, and the log counts a coin.
3. **HV on for 10 s**, still with J14 off: about 20 V on the rail and 19 V on HV. Check both with a
   meter.
4. A 100 Ω 5 W resistor (or a 12-24 V coil) on J11: Collect and Return should read a current.
5. Fit J14 (52 V) and the coin relay: HV on, then Collect and Return. Adjust the pulse length
   if the relay doesn't move reliably.

## See also

- [Design notes](https://github.com/sdylewski/Analog-Redial/blob/main/PayPhone/coin-board-schematic.md):
  every part value and how each stage works
- [Output stage, pin by pin](https://github.com/sdylewski/Analog-Redial/blob/main/PayPhone/output-stage-wiring.md)
- [Parts, ordering and layout](https://github.com/sdylewski/Analog-Redial/blob/main/PayPhone/coin-board-smt.md)
- [Payphone types](https://github.com/sdylewski/Analog-Redial/blob/main/PayPhone/payphone-types.md):
  what each known payphone needs
- [Schematic (PDF)](https://github.com/sdylewski/Analog-Redial/blob/main/PayPhone/Payphone_coin_board_v1/Payphone_coin_board_v1_schematic.pdf)
- [Board reference: v6](hardware-smd6.md), whose J11 header the coin board plugs into
