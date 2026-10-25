# Analog Redial v6

**Released, not tested yet.** It should work, but no v6 board has been built.
**v5** (`../Analog Redial v5/`) is the board that has been tested; **v5.1**
(`../Analog Redial v5.1/`) is v5 with the same clone-pin fix, also not built yet.

What it changes from v5:

- The clone-module footprint is corrected. On v5 the 30-pin clone's
  bottom-right 5V pin is wired to GND; v6 fixes that, so there is no pin to
  remove.
- Silkscreen updates.

What it does not change: the expansion header's (J2's) regulator is still the 3.3 V
XC6206P332MR (a 3.6 V part was considered and not done), and the rest of the
design is v5's.

Fabrication files: `pcbway_production/` (PCBWay) and `jlcpcb/` (JLCPCB: Gerbers,
BOM and placement files).
