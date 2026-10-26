# Payphone instruction cards

Editable cards in the style of the paper inserts behind the plastic windows on a payphone:
an **upper** card (the brand, rates and services) and a **lower** card (dialing instructions).
They are plain SVG, so you can open them in Inkscape, Affinity, Illustrator, or a text editor.

| File | What it is |
|--|--|
| `upper_card_example_rates.svg` | Upper card: a picture on the left, and a brand name with three services on the right (local calls, operator, emergencies). |
| `upper_card_example_steps.svg` | Upper card: a blue band with four numbered steps (handset, keypad, answer, coin), the steps written out, and an emergency line. |
| `lower_card_example_dialing.svg` | Lower card: a "Dialing Instructions" header, ruled sections of instructions, a keypad-style `0+` and `1+`, and a box for a carrier name or logo. |

All the text is in the examples as placeholders ("YOUR NAME", the address, the prices): replace them.
The calls and numbers are just examples, so write what your phone actually does.

`png/` holds 300 dpi PNG exports of the same three cards (about 2100 px wide), for GIMP or for printing
straight away. Edit the SVG, then export again; for example with `rsvg-convert -d 300 -p 300 file.svg -o png/file.png`.

## Editing

- **Text and colours:** each file starts with a `<style>` block; change a colour or a font size once
  and it changes everywhere it's used. The fonts are common ones (Helvetica Neue, Arial); if you have
  a condensed face like Arial Narrow or Franklin Gothic, it looks closer to the originals.
- **Pictures:** the dial and the step icons are simple shapes inside named groups
  (`id="dial-art"`, `id="step1"` ...). Delete them, or import your own picture on top.
- **Size:** the cards are 178 x 64 mm (upper) and 178 x 84 mm (lower), about 7.0 x 2.5 in and
  7.0 x 3.3 in. **Measure your phone's card slot** (the visible window, plus the paper's edge under the
  frame) and change `width` and `height` in the first line of the file; the drawing is scaled by its
  `viewBox`, so it stretches to fit. Keep text away from the outer 3 mm, since the frame covers it.

## Printing

1. Export a PDF from your editor (or use the SVG directly), and print at **100% / Actual size**,
   not "Fit to page".
2. Use matte photo paper or 160-200 gsm card for a stiff, glare-free insert. For a back-lit look,
   print on a thin white film instead.
3. Cut on the outside edge (the cards have a thin rounded border you can trim to), then slide it
   into the slot behind the window.

## Where to find real ones

[payphone411.com's instruction cards](http://payphone411.com/instructioncards.html) shows scans of
many real cards (Bell, NYNEX, Verizon, Pacific Bell, ...). These examples are original drawings in the
same style, not copies. Don't reuse the scans' logos or artwork in anything you publish.
