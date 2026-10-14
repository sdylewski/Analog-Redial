[Home](../README.md) · [1 · Get a board](board.md) · **2 · Parts & assembly** · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md)

# 2 · Assembly

From a fab-assembled board and the [parts list](bom.md) to a board that's
ready to flash. Everything you solder is through-hole, so a regular iron is
all you need. Written for v4; v5 differences are called out where they come
up.

## 1. Tools

- Soldering iron with a fine conical or chisel tip, and solder
- Flush cutters for trimming leads
- Multimeter (continuity and DC volts). **Not optional**, see [§5](#5-check-before-power)
- A 2 A USB-C charger, or a 5.5–6 V DC supply
- One or two analog telephones with RJ11/RJ12 cords

## 2. v4 only: remove two resistors first

**Remove R14 and R12**, the two 15 kΩ resistors in the hook-switch dividers.
With them fitted, the off-hook signal only reaches about 1.9 V, below what
the ESP32-C6 reliably reads as high. The symptom is a line whose audio works
but which never notices the handset being lifted.

Leave **R15** and **R13**, the 10 kΩ resistors, in place. Full explanation
in the [v4 board reference](hardware-smd4.md#7-shk-divider-rework-smd4-as-built).
v5 has the corrected values, so there's nothing to remove.

## 3. Soldering order

Lowest parts first, tallest last.

| Order | Ref | Part | Notes |
|-------|-----|------|-------|
| 1 | **J5** | 2×4 pin header | Lowest profile, do it first |
| 2 | **J9** | RJ12 jack, line 0 | Snaps in; check it sits flat |
| 3 | **J8** | RJ12 jack, line 1 | Second line only |
| 4 | **D12** | P6KE82A TVS | Optional. **Polarized**: the stripe goes at the silkscreen band end, toward `/P6K`, not GND |
| 5 | **C16** | 470 µF, line 0 | **Polarized**: stripe to the negative pad |
| 6 | **C17** | 470 µF, line 1 | Second line only, same polarity care |
| 7 | **U4** | AG1171 SLIC, line 0 | 14-pin module, stands upright. Match pin 1 to the silkscreen **before** soldering: desoldering a 14-pin SIL module is miserable |
| 8 | **U3** | AG1171 SLIC, line 1 | Second line only |
| 9 | **J1** | DC barrel jack | Optional; chunky, needs a hot iron and a moment to wet |
| 10 | **U1** | ESP32-C6 DevKit | Tallest part, so it goes last |

**Keeping the DevKit square:** tack one pin at each far corner, check the
module sits flat and square against the board, then solder the rest.

**v5 Bluetooth header (J2)**, when that option arrives: fit it before the
DevKit.

## 4. Set the build jumpers

The board has four spare pins that can do one job at a time, and the
**J5** jumpers choose which:

| | Second phone line | microSD card | Your own gadgets |
|---|---|---|---|
| Parts fitted | **U3**, **J8**, **C17** | Nothing extra | Nothing extra |
| **J5** | **Jumpers on**: bridge all four pairs | **Jumpers off**, card in the slot | **Jumpers off**, your wires on pins 1, 3, 5 and 7 ([which pin is which](bom.md#build-options)) |

Never a mix: all four jumpers on, or none. If you get it wrong the firmware will tell you: a card in a
board set up for two lines shuts line 1 off and shows a warning in the web
interface.

## 5. Check before power

> ⚠️ **This board makes real telephone-line voltages:** about 48 V DC on an
> off-hook line and about 60 V AC while ringing, with peaks near 90 V.
> Don't touch the RJ12 pins or the SLIC pins while it's powered, power down
> before changing jumpers, and never connect the jacks to house or telco
> wiring.

Five minutes with a multimeter beats a dead SLIC:

1. **Look for bridges** between neighbouring pins, especially on the RJ12
   jacks and the DevKit headers.
2. **Capacitor polarity:** C16/C17 stripe to the negative pad. Backwards is
   the classic way to make one vent.
3. **No shorts:** +5 V to GND and 3.3 V to GND should both read open.
4. **J5:** all four bridged for two lines, all open for microSD.

## 6. First power-on

Power it with **nothing plugged into the phone jacks yet**.

- The DevKit's LED should come on. Nothing should get hot; if anything does,
  pull the power and go back to [§5](#5-check-before-power).
- Measure the 5 V and 3.3 V rails. Both should be within a few percent.
- Only then plug in a phone.

The board has no firmware yet, so it won't do anything useful. That's
expected.

---

**Next: [3 · Flash & test](flashing.md)**
