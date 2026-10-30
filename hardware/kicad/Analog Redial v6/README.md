# Analog Redial v6

**Released, not tested yet.** It should work, but no v6 board has been built.
**v5** (`../Analog Redial v5/`) is the board that has been tested; **v5.1**
(`../Analog Redial v5.1/`) is v5 with the same clone-pin fix, also not built yet.

What it changes from v5:

- The clone-module footprint is corrected. On v5 the 30-pin clone's
  bottom-right 5V pin is wired to GND; v6 fixes that, so there is no pin to
  remove.
- **D1, the diode that joins the DC jack to the +5 V rail, is an SS34 (3 A,
  JLCPCB C8678) instead of v5's SS14 (1 A).** Two phone lines in use, with Wi-Fi
  and Bluetooth, can draw 1 A or more, which is beyond the SS14's rating. Same
  SMA package, so nothing else changes.
- Larger vias on the power nets: 13 of the 17 vias on +5 V and +3.3 V are 0.8 mm
  pad / 0.4 mm drill (v5's are all 0.6 / 0.3), and the Power net class uses
  0.8 / 0.4 for new ones. Four that didn't fit beside other parts stayed 0.6 / 0.3.
- **J6, a new 5-pin I²C header** (2.54 mm, 1 x 5) for add-on boards:

  | J6 pin | Net |
  |---|---|
  | 1 | +5 V |
  | 2 | +3.3 V |
  | 3 | I²C SDA |
  | 4 | I²C SCL |
  | 5 | GND |

  It is the same I²C bus as the ES8388 codec (address 0x10), with the main
  board's 4.7 kΩ pull-ups, so give an add-on a different address. For a cable
  of up to about 1 m, run the bus at 100 kHz, not 400 kHz, and twist SDA and
  SCL with ground wires.
- **U5, the expansion header's (J2's) regulator, is the 3.6 V XC6206P362MR
  (JLCPCB C351446)** instead of v5's 3.3 V XC6206P332MR, so J2 pin 1 is 3.6 V.
- Teardrops on pads and vias.
- Silkscreen updates.

What it does not change: the rest of the design is v5's.

Fabrication files: `pcbway_production/` (PCBWay) and `jlcpcb/` (JLCPCB: Gerbers,
BOM and placement files).
