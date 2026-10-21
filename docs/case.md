[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](building.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md) · **Case**

# 3D-printed case

A two-part case, a box and a lid, that you print yourself. The boards are
different lengths, so **each board version has its own case**: a v4 case
will not fit a v5.

<!-- TODO: photo of the finished board in its case (docs/pics/board_in_case.jpg) -->
![Photo coming soon: the board in its case](assets/photo-placeholder.svg)

## Which case do I print?

| Board | Box | Lid | Status |
|-------|-----|-----|--------|
| **v4**, single line | `Box-SingleLine` | `Lid` | Ready |
| **v4**, dual line | `Box - dualLine` | `Lid` | Ready |
| **v5** (5 mm longer than v4) | Not yet | Not yet | In progress |

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

### v5

The v5 case files will go in
[`hardware/3d-printed-case/Analog Redial v5/`](https://github.com/sdylewski/Analog-Redial/tree/main/hardware/3d-printed-case/Analog%20Redial%20v5)
once they are printed and tested.

<!-- TODO: v5 STEP and STL links, and photos, when the files exist -->

## What you need

- **4 × M3 screws, 5–6 mm long**, to mount the board in the box.
- A little **3D-printer filament** to attach the lid (see below).

## Assembling it

1. Set the board in the box and fasten it with the four M3 screws. Use the
   short screws: a longer one can poke through the underside of the box.
2. Fit the lid on.
3. Attach the lid with filament.

   <!-- TODO: describe how the filament holds the lid (pin, melted rivet, ...) and add a photo -->

<!-- TODO: photo of the board mounted in the open box -->

## Editing the design

The cases are designed in OnShape. The STEP files open in any CAD program.

<!-- TODO: link to the OnShape document, once it is made public -->

Made a better case, or one for a different phone? See
[CONTRIBUTING](https://github.com/sdylewski/Analog-Redial/blob/main/CONTRIBUTING.md).
