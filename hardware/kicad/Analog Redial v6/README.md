# Analog Redial v6

**Released, not built or tested yet.** The KiCad project, a schematic PDF and the
JLCPCB fab files (`jlcpcb/`: gerbers, BOM and placement) are here, but no v6 board has
been built and its firmware support is untested. **v5** (`../Analog Redial v5/`) is
the tested board and **v5.1** (`../Analog Redial v5.1/`) is v5 with the
clone-module 5V pin fixed. Board reference: `docs/hardware-smd6.md`.

What v6 adds:

- **Everything at once:** two phone lines, the microSD card, payphone
  support and extra I/O all work together. On v4 and v5 the second line and
  the microSD card share the same four ESP32 pins, so a board has one or the
  other.
- **An I²C I/O expander** (TI TCA9534A, address 0x38) drives the second
  line's SLIC, which frees those ESP32 pins for the microSD card. Its four
  spare pins (P4-P7) come out on header J10 as extra I/O: the firmware drives
  them from pin dial codes (Settings → Dial Codes → Pin outputs), whatever the
  lines and microSD card are set to.
- **Payphone support:** a 5-pin I²C accessory header (1 GND, 2 SCL, 3 SDA,
  4 +3.3 V, 5 +5 V) for add-on boards such as the payphone coin board, itself
  sent to fab but untested (its firmware is in Settings → Payphone → Coin board).
- The clone-module footprint is corrected, so there is no pin to remove.
- A 3 A diode (SS34) between the DC jack and the +5 V rail instead of v5's
  1 A SS14.
- Larger vias on the power nets, teardrops, and updated silkscreen.
