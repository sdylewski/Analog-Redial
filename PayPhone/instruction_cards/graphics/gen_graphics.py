"""Generates the payphone card graphics (PayPhone/instruction_cards/graphics/).

Text is converted to outlines (Helvetica Neue Condensed / Bold), so each SVG looks the
same in any program, including OpenOffice/LibreOffice, without the font installed.
Usage: python gen_graphics.py OUT_DIR
"""
import math
import sys
from fontTools.ttLib import TTCollection
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

OUT = sys.argv[1]
TTC = '/System/Library/Fonts/HelveticaNeue.ttc'
FONTS = {f['name'].getDebugName(4): f for f in TTCollection(TTC).fonts}

BLUE = '#1f4fa8'
RED = '#d6202c'
WHITE = '#ffffff'


def text_path(s, font, size, x, y, tracking=0.0, anchor='start'):
    """SVG path data for `s` set in `font` at `size` px, baseline at y."""
    f = FONTS[font]
    gs = f.getGlyphSet()
    cmap = f.getBestCmap()
    upm = f['head'].unitsPerEm
    k = size / upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width for n in names) * k + tracking * (len(s) - 1)
    if anchor == 'middle':
        x -= width / 2
    elif anchor == 'end':
        x -= width
    pen = SVGPathPen(gs, ntos=lambda v: ('%.2f' % v).rstrip('0').rstrip('.'))
    cx = x
    for n in names:
        tp = TransformPen(pen, (k, 0, 0, -k, cx, y))
        gs[n].draw(tp)
        cx += gs[n].width * k + tracking
    return pen.getCommands(), width


def write(name, w, h, body, comment):
    with open(f'{OUT}/{name}', 'w') as fh:
        fh.write(f'<?xml version="1.0" encoding="UTF-8"?>\n<!-- {comment} -->\n'
                 f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
                 f'viewBox="0 0 {w} {h}">\n{body}</svg>\n')


# ---- rotary dial -------------------------------------------------------------
def dial(cx, cy, r, disc, ink):
    """A rotary dial: a disc, a ring of ten finger holes with the usual gap, a hub
    and a finger stop. Angles are clockwise from 12 o'clock."""
    out = [f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{disc}"/>']
    ring = r * 0.72
    hole = r * 0.135
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{r * 0.9:.2f}" fill="none" stroke="{ink}" '
               f'stroke-width="{r * 0.07:.2f}"/>')
    for i in range(10):                      # "1" at 60 deg, then 30 deg steps back round
        a = math.radians(60 - 30 * i)
        out.append(f'<circle cx="{cx + ring * math.sin(a):.2f}" cy="{cy - ring * math.cos(a):.2f}" '
                   f'r="{hole:.2f}" fill="{ink}"/>')
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{r * 0.26:.2f}" fill="{ink}"/>')
    a = math.radians(118)                    # finger stop, in the gap after "1"
    x1, y1 = cx + r * 0.58 * math.sin(a), cy - r * 0.58 * math.cos(a)
    x2, y2 = cx + r * 0.98 * math.sin(a), cy - r * 0.98 * math.cos(a)
    out.append(f'<path d="M{x1:.2f} {y1:.2f}L{x2:.2f} {y2:.2f}" stroke="{ink}" '
               f'stroke-width="{r * 0.11:.2f}" stroke-linecap="round"/>')
    return '\n'.join('  ' + l for l in out) + '\n'


write('rotary_dial_red.svg', 200, 200, dial(100, 100, 96, RED, WHITE),
      'Rotary dial logo: red disc, white dial.')
write('rotary_dial_white.svg', 200, 200, dial(100, 100, 96, WHITE, RED),
      'Rotary dial logo, reversed: white disc, red dial (for a red or blue background).')
write('rotary_dial_blue.svg', 200, 200, dial(100, 100, 96, BLUE, WHITE),
      'Rotary dial logo in the card blue.')


# ---- PACIFIC (dial) REDIAL ---------------------------------------------------
def logo(word_fill, dial_disc, dial_ink, bg=None):
    size = 100
    p1, w1 = text_path('PACIFIC', 'Helvetica Neue Condensed Black', size, 0, size * 0.72 + 6)
    gap = 18
    d = size * 0.86                     # dial diameter, a little over the cap height
    dx = w1 + gap + d / 2
    p2, w2 = text_path('REDIAL', 'Helvetica Neue Condensed Black', size, w1 + gap * 2 + d,
                       size * 0.72 + 6)
    W = w1 + gap * 2 + d + w2
    H = size * 0.72 + 12
    pad = 10 if bg else 0
    body = ''
    if bg:
        body += f'  <rect width="{W + 2 * pad:.1f}" height="{H + 2 * pad:.1f}" fill="{bg}"/>\n'
    body += f'  <g transform="translate({pad} {pad})">\n'
    body += f'  <path fill="{word_fill}" d="{p1}"/>\n'
    body += dial(round(dx, 2), round(6 + size * 0.36, 2), round(d / 2, 2), dial_disc, dial_ink)
    body += f'  <path fill="{word_fill}" d="{p2}"/>\n  </g>\n'
    return W + 2 * pad, H + 2 * pad, body


for name, args, note in [
    ('pacific_redial_logo.svg', (RED, RED, WHITE), 'red, for a white card'),
    ('pacific_redial_logo_white.svg', (WHITE, WHITE, BLUE, BLUE), 'white on the card blue'),
    ('pacific_redial_logo_blue.svg', (BLUE, BLUE, WHITE), 'blue, for a white card'),
]:
    w, h, body = logo(*args)
    write(name, round(w, 1), round(h, 1), body,
          f'"PACIFIC (rotary dial) REDIAL" logo, {note}. Letters are outlines '
          '(Helvetica Neue Condensed Black), so it looks the same everywhere.')


# ---- Phone sign --------------------------------------------------------------
HANDSET = (
    # earpiece, handle and mouthpiece as one shape, in a 360 x 360 sign
    'M 70 28 L 126 34 C 134 35 137 42 134 49 L 108 114 C 105 120 99 122 93 121 L 84 120 '
    'C 76 140 72 160 72 180 C 72 200 76 220 84 240 L 93 239 C 99 238 105 240 108 246 '
    'L 134 311 C 137 318 134 325 126 326 L 70 332 '
    'C 36 300 18 240 18 180 C 18 120 36 60 70 28 Z'
)
# the two thin lines that separate the cups from the handle
SEAMS = 'M 97 32 L 82 121 M 97 328 L 82 239'


def sign(bg, ink, border=None):
    p, _ = text_path('Phone', 'Helvetica Neue Bold', 76, 226, 207, anchor='middle')
    b = f'  <rect x="4" y="4" width="352" height="352" rx="34" fill="{bg}"'
    b += f' stroke="{border}" stroke-width="8"/>\n' if border else '/>\n'
    b += f'  <path fill="{ink}" d="{HANDSET}"/>\n'
    b += f'  <path d="{SEAMS}" stroke="{bg}" stroke-width="4" stroke-linecap="round"/>\n'
    b += f'  <path fill="{ink}" d="{p}"/>\n'
    return b


write('phone_sign_blue.svg', 360, 360, sign(BLUE, WHITE),
      'Public "Phone" sign: white handset and lettering on blue.')
write('phone_sign_white.svg', 360, 360, sign(WHITE, BLUE, BLUE),
      'Public "Phone" sign, reversed: blue handset and lettering on white, with a blue border.')
