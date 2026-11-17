[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](building.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md) · **Case**

# 3D-printed case

A two-part case, a box and a lid, that you print yourself. The boards are
different lengths, so **each board version has its own case**: a v4 case
will not fit a v5 or v6.

<!-- TODO: photo of the finished board in its case (docs/pics/board_in_case.jpg) -->
![A v4 board in its 3D-printed case](pics/v4_case.jpg)
*A v4 board in its case*

## Which case do I print?

| Board | Box | Lid | Status |
|-------|-----|-----|--------|
| **v4**, single line | `Box-SingleLine` | `Lid` | Ready |
| **v4**, dual line | `Box - dualLine` | `Lid` | Ready |
| **v5** and **v6**, one line (5 mm longer than v4) | `Box-OneLine-130x70x35` | `Lid-FlatHead-130x70` | First files available, below. v6 has the same outline and mounting holes as v5, so it should fit; not tried on a v6 yet |

Print one box and one lid. The lid is the same for single-line and dual-line
v4 boards.

## Files

Each part comes as a **STEP** file (for editing in CAD) and an **STL** file
(for slicing and printing).

### v4

| Part | STEP | STL |
|------|------|-----|
| Box, single line | [.step](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v4/AnalogRedial-4%20-%20Box-SingleLine.step) | [.stl](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v4/AnalogRedial-4%20-%20Box-SingleLine.stl) |
| Box, dual line | [.step](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v4/AnalogRedial-4%20-%20Box-%20DualLine.step) | [.stl](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v4/AnalogRedial-4%20-%20Box%20-%20dualLine.stl) |
| Lid | [.step](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v4/AnalogRedial-4%20-%20Lid.step) | [.stl](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v4/AnalogRedial-4%20-%20Lid.stl) |

<!-- TODO: photos of the v4 box (single-line and dual-line) and the lid -->

### v5 and v6

In [`hardware/3d-printed-case/Analog Redial v5/`](https://github.com/sdylewski/Analog-Redial/tree/main/hardware/3d-printed-case/Analog%20Redial%20v5),
for a one-line board. The outside is 130 × 70 × 35 mm. The box also comes as a `.3mf`
for slicers that take it.

| Part | STEP | STL |
|------|------|-----|
| Box, one line | [.step](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v5/AnalogRedial-5%20-%20Box-OneLine-130x70x35.step) | [.stl](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v5/AnalogRedial-5%20-%20Box-OneLine-130x70x35.stl) |
| Lid | [.step](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v5/AnalogRedial-5%20-%20Lid-FlatHead-130x70.step) | [.stl](https://github.com/sdylewski/Analog-Redial/blob/main/hardware/3d-printed-case/Analog%20Redial%20v5/AnalogRedial-5%20-%20Lid-FlatHead-130x70.stl) |

<!-- TODO: a dual-line box, and photos -->

## What you need

- **4 × M3 screws, 5–6 mm long**, to mount the board in the box.
- A short piece of **3D-printer filament** (about 1.75 mm) for the hinge pin.

## Assembling it

1. Set the board in the box and fasten it with the four M3 screws. Use the
   short screws: a longer one can poke through the underside of the box.
2. Line up the lid's hinge with the box.
3. Push a piece of filament through the hinge as the pin, and trim it flush.

<!-- TODO: photos of the board in the open box and of the filament hinge pin -->

## Editing the design

The cases are designed in OnShape. The STEP files open in any CAD program.

<!-- TODO: link to the OnShape document, once it is made public -->

Made a better case, or one for a different phone? See
[CONTRIBUTING](https://github.com/sdylewski/Analog-Redial/blob/main/CONTRIBUTING.md).
