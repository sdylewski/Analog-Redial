# Analog Redial v5 (as fabbed)

These are the KiCad files for the **v5 boards that were actually built**: the
board file is the 2026-10-05 07:00 save that produced the fab package, and
the schematic and project file match it. Nothing here has been changed since,
so it stays a faithful record of what is on the first v5 boards.

- **Fab package as sent:** `Analog Redial v5 - as sent to fab 2026-10-05.zip`
  (Gerbers, drill files, bill of materials, positions and netlist, exported
  for PCBWay).
- **Known error:** on a 30-pin clone ESP32-C6 module the footprint ties the
  clone's bottom-right 5V pin to GND. Remove that pin when fitting a clone
  (see `docs/board.md`). The genuine 32-pin DevKitC-1 is not affected.
- **The corrected design is `../Analog Redial v5.1/`**: this board with the
  clone 5V-pin footprint fixed, plus small silkscreen and fab-file updates
  (JLCPCB export included). Not built yet. Edit and re-export from v5.1, not
  from here.
- **History:** the Oct 7 continuation of v5 (silkscreen label, a few 1.0 mm
  holes) is in git commit `19def2e` as `Analog Redial 5.*`; it was not what
  was fabbed.
