# 3D-printed case

A two-part case, box and lid, for the Analog Redial board. The build guide
(which one to print, screws, assembly) is the
[case page](../../docs/case.md).

<!-- TODO: replace with a photo of the board in its case, e.g. ../../docs/pics/board_in_case.jpg -->
![Photo coming soon: the board in its 3D-printed case](../../docs/assets/photo-placeholder.svg)

## v4 board

In [`Analog Redial v4/`](Analog%20Redial%20v4/). Print one box and the lid.

| File | Print it for |
|------|--------------|
| `AnalogRedial-4 - Box-SingleLine.stl` | Single-line board |
| `AnalogRedial-4 - Box - dualLine.stl` | Dual-line board |
| `AnalogRedial-4 - Lid.stl` | Either |

The `.step` files are the same parts, for editing in CAD.

## v5 and v6 boards

**v5 and v6 boards both use the v5+ cases.** They are in [`Analog Redial v5+/`](Analog%20Redial%20v5%2B/), with `STEP/` and `STL/` folders. v5 is 5 mm
longer than v4, so the v4 case won't fit it. v6 has the same outline and mounting holes as v5,
so this case should fit it too (not tried yet). Print one box and one lid.

| File | Print it for |
|---|---|
| `AnalogRedial-v5 - Box-1Line-32pinESP32` | One line, real ESP32-C6-DevKitC-1 |
| `AnalogRedial-v5 - Box-1Line-30pinESP32` | One line, 30-pin clone module |
| `AnalogRedial-v5 - Box-2Lines-32pinESP32` | Two lines, real ESP32-C6-DevKitC-1 |
| `AnalogRedial-v5 - Box-2Lines-30pinESP32` | Two lines, 30-pin clone module |
| `AnalogRedial-v5 - Lid-withLogo` | Any box, dial logo on the lid |
| `AnalogRedial-v5 - Lid-noLogo` | Any box, plain lid |

**The USB hole:** the 30-pin clone's USB-C socket is recessed from the board edge, so the
`30pinESP32` boxes have a larger hole for the cable plug to reach it. The `32pinESP32` boxes
have the smaller hole that fits the real DevKitC.

Each is a `.stl` (to print) and a `.step` (to edit in CAD).
