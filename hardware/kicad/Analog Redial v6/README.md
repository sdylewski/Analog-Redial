# Analog Redial v6

**In progress.** The design files will be published here once the new
features below are finished. Until then, **v5** (`../Analog Redial v5/`) is
the tested board and **v5.1** (`../Analog Redial v5.1/`) is v5 with the
clone-module 5V pin fixed.

What v6 adds:

- **Everything at once:** two phone lines, the microSD card, payphone
  support and extra I/O all work together. On v4 and v5 the second line and
  the microSD card share the same four ESP32 pins, so a board has one or the
  other.
- **An I²C I/O expander** (TI TCA9534A, address 0x38) drives the second
  line's SLIC, which frees those ESP32 pins for the microSD card. Its four
  spare pins come out on a header as extra I/O.
- **Payphone support:** a 5-pin I²C accessory header (+5 V, +3.3 V, SDA, SCL,
  GND) for add-on boards such as the payphone coin board.
- The clone-module footprint is corrected, so there is no pin to remove.
- A 3 A diode (SS34) between the DC jack and the +5 V rail instead of v5's
  1 A SS14.
- Larger vias on the power nets, teardrops, and updated silkscreen.
