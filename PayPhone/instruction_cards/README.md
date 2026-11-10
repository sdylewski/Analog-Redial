# Payphone instruction cards

Editable cards in the style of the paper inserts behind the plastic windows on a payphone:
an **upper** card (the brand, rates and services) and a **lower** card (dialing instructions).
They are plain SVG, so you can open them in Inkscape, Affinity, Illustrator, or a text editor.

| File | What it is |
|---|---|
| `upper_card_example_rates.svg` | Upper card: a picture on the left, and a brand name with three services on the right (local calls, operator, emergencies). |
| `upper_card_example_steps.svg` | Upper card: a blue band with four numbered steps (handset, keypad, answer, coin), the steps written out, and an emergency line. |
| `lower_card_example_dialing.svg` | Lower card: a "Dialing Instructions" header, ruled sections of instructions, a keypad-style `0+` and `1+`, and a box for a carrier name or logo. |
| `upper_card_pacific_redial.svg` | Upper card in the blue-and-white style of the 1990s West Coast cards: free-call and how-to-call bands, dotted-leader free numbers, an offer box, and generic payment badges (no real card logos). |
| `lower_card_pacific_redial.svg` | Matching lower card for a made-up company, "Pacific Redial": red wordmark with a rotary-dial logo, a deposit box, three columns of dialing instructions, a large 9-1-1 and an address line. All numbers are 555 placeholders. |

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
- **Size:** see [Card sizes](#card-sizes). The two Pacific Redial cards are drawn at the G-Tel sizes,
  in 1/100 in units; the three older examples are 178 x 64 mm and 178 x 84 mm. To fit your phone,
  change `width` and `height` in the first line of the file; the drawing is scaled by its `viewBox`,
  so it stretches to fit. Keep text away from the outer 1/8 in (3 mm), since the frame covers it.

## Card sizes

There's no single published standard: the cards are "fairly standard in size" between housings, and
people who make them size them against the card already in their phone. The one stated size is G-Tel's
replacement set (card and clear cover, phone models not stated):

| Card | Size | Metric |
|---|---|---|
| Upper (top) card | 6 1/8 x 1 7/8 in | 156 x 48 mm |
| Lower card | 6 1/8 x 2 1/2 in | 156 x 64 mm |

Other cards on 3-slot phones (Western Electric style, from Old Phone Shop's listings): the middle
direction card is 3 7/8 x 3 3/8 in and the top flag card 6 1/4 x 4 1/4 in.

Scans of 1990s Pacific Bell cards have the same width-to-height ratio as G-Tel's upper card (about 3.2:1);
their lower card is a little taller (about 2.2:1, so about 2 3/4 in at 6 1/8 in wide). **Measure your
phone's slots** before printing: the visible window, plus the edge of the paper under the frame. The
G-Tel plastic covers have been reported slightly too large for some housings and are trimmed to fit.

Sources: [G-Tel instruction card set](https://payphone.com/Instruction-Card-Set.html),
[PhilTel: creating new payphone instruction cards](https://philtel.org/2022/11/14/creating-new-payphone-instruction-cards.html),
[Old Phone Shop middle card](https://www.oldphoneshop.com/products/yellow-3-slot-payphone-middle-instruction-card.html),
[Old Phone Shop flag card](https://www.oldphoneshop.com/products/payphone-flag-card-and-plastic-for-3-slot-pay-phone.html).

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
