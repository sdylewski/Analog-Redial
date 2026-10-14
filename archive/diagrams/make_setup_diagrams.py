#!/usr/bin/env python3
"""Writes the editable part icons and the composed setup diagrams in docs/assets.

  docs/assets/parts/<name>.svg      one item each, transparent, own tight viewBox
  docs/assets/parts/png/<name>.png  the same at 2x, for GIMP / Keynote
  docs/assets/setup-*.svg           a composed diagram; every item is a named
                                    group (Inkscape: Layers and Objects panel)
  docs/assets/setup-*.png           the composed diagram at 2x

Run from anywhere:  python3 archive/diagrams/make_setup_diagrams.py
Needs rsvg-convert (brew install librsvg) for the PNGs; the SVGs need nothing.

The palette and the phone/board/cord shapes follow docs/assets/use-*.svg.
Edit PARTS or a compose_*() function and re-run, or edit the SVGs by hand:
the generator never reads them back.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "docs" / "assets"
PARTS = ASSETS / "parts"

# palette (from the existing use-*.svg)
CREAM, INK, OLIVE, OLIVE_D = "#f7f0e0", "#2b2620", "#7c8c45", "#4d5a26"
AMBER, AMBER_D = "#c78a2b", "#9c6a1f"
TAN, BROWN, BROWN_D, TEXT = "#8a6f4d", "#5c4a35", "#423525", "#5c4a35"
PAPER, PAPER_D = "#efe4cb", "#d6c7a4"
GREEN_ARC = "#5f7a2a"
FONT = "system-ui, -apple-system, Segoe UI, Roboto, sans-serif"

# ---------------------------------------------------------------- parts
# name -> (width, height, label, svg markup drawn from 0,0)

def cord(length: float, waves: int = 4) -> str:
    seg = length / waves
    d = f"M 0 0 " + " ".join(
        f"q {seg/2:.2f} {-12 if i % 2 == 0 else 12} {seg:.2f} 0" for i in range(waves))
    # one up-bump then one down-bump per pair, matching the existing cords
    d = "M 0 0 " + " ".join(
        f"q {seg/2:.2f} {-12 if i % 2 == 0 else 12} {seg/2:.2f} 0" for i in range(waves * 2))
    return f'<path d="{d}" fill="none" stroke="{TAN}" stroke-width="4" stroke-linecap="round"/>'


PARTS_DEF: dict[str, tuple[float, float, str, str]] = {}


def part(name: str, w: float, h: float, label: str, body: str) -> None:
    PARTS_DEF[name] = (w, h, label, body)


part("phone", 90, 80, "Analog telephone", f'''
  <g id="handset"><line x1="14" y1="16" x2="76" y2="16" stroke="{OLIVE_D}" stroke-width="20" stroke-linecap="round"/>
    <circle cx="14" cy="16" r="13" fill="{OLIVE}"/><circle cx="76" cy="16" r="13" fill="{OLIVE}"/></g>
  <g id="body"><rect x="4" y="32" width="82" height="44" rx="8" fill="{INK}"/>
    <circle cx="45" cy="54" r="14" fill="none" stroke="{OLIVE}" stroke-width="3"/>
    <circle cx="45" cy="54" r="3.5" fill="{OLIVE}"/></g>''')

part("board", 220, 190, "Analog Redial board (two lines)", f'''
  <rect id="pcb" x="0" y="0" width="220" height="190" rx="10" fill="{INK}"/>
  <rect id="jack-line-1" x="-7" y="24" width="10" height="26" rx="2" fill="{BROWN}"/>
  <rect id="jack-line-2" x="-7" y="140" width="10" height="26" rx="2" fill="{BROWN}"/>
  <circle id="led-1" cx="30" cy="16" r="4" fill="{AMBER}"/><circle id="led-2" cx="46" cy="16" r="4" fill="{OLIVE}"/>
  <text x="110" y="100" text-anchor="middle" font-size="17" font-weight="600" fill="{CREAM}">ANALOG REDIAL</text>
  <text x="110" y="122" text-anchor="middle" font-size="12" fill="{TAN}">two lines</text>''')

part("bluetooth-module", 40, 18, "Bluetooth module tag (BM64)", f'''
  <rect x="0" y="0" width="40" height="18" rx="4" fill="{AMBER_D}"/>
  <text x="20" y="13" text-anchor="middle" font-size="10" font-weight="600" fill="#f5e4bf">BM64</text>''')

part("bluetooth-symbol", 16, 24, "Bluetooth rune", f'''
  <path d="M 1 7 L 15 17 L 8 23 L 8 1 L 15 7 L 1 17" fill="none" stroke="{AMBER}" stroke-width="3"
        stroke-linecap="round" stroke-linejoin="round"/>''')

part("cell-phone", 62, 112, "Mobile phone", f'''
  <rect x="0" y="0" width="62" height="112" rx="10" fill="{INK}"/>
  <rect x="6" y="11" width="50" height="84" rx="4" fill="#3a3128"/>
  <circle cx="31" cy="103" r="4" fill="{BROWN}"/>''')

part("cellular", 44, 80, "Cellular signal arcs", f'''
  <path d="M 0 56 A 30 30 0 0 0 0 0" fill="none" stroke="{GREEN_ARC}" stroke-width="5" stroke-linecap="round"/>
  <path d="M 8 66 A 48 48 0 0 0 8 -10" fill="none" stroke="{GREEN_ARC}" stroke-width="5" stroke-linecap="round"
        transform="translate(0,0)"/>''')

part("wifi", 60, 40, "Wi-Fi", f'''
  <path d="M 6.4 22.5 A 34 34 0 0 1 53.6 22.5" fill="none" stroke="{AMBER}" stroke-width="5" stroke-linecap="round"/>
  <path d="M 14.5 29.4 A 23 23 0 0 1 45.5 29.4" fill="none" stroke="{AMBER}" stroke-width="5" stroke-linecap="round"/>
  <path d="M 22.4 36.2 A 12 12 0 0 1 37.6 36.2" fill="none" stroke="{AMBER}" stroke-width="5" stroke-linecap="round"/>
  <circle cx="30" cy="40" r="4.5" fill="{AMBER}" transform="translate(0,-2)"/>''')

part("browser", 84, 62, "Web interface (browser window)", f'''
  <rect x="0" y="0" width="84" height="62" rx="7" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>
  <path d="M 0 14 H 84 V 7 a 7 7 0 0 0 -7 -7 H 7 a 7 7 0 0 0 -7 7 Z" fill="{PAPER_D}"/>
  <circle cx="9" cy="7" r="2.6" fill="{AMBER}"/><circle cx="18" cy="7" r="2.6" fill="{OLIVE}"/>
  <rect x="8" y="22" width="30" height="6" rx="3" fill="{OLIVE}"/>
  <rect x="8" y="34" width="68" height="4" rx="2" fill="{PAPER_D}"/>
  <rect x="8" y="43" width="68" height="4" rx="2" fill="{PAPER_D}"/>
  <rect x="8" y="52" width="44" height="4" rx="2" fill="{PAPER_D}"/>''')

part("server", 76, 62, "VoIP server (Asterisk / PBX)", f'''
  <rect x="0" y="0" width="76" height="28" rx="6" fill="{INK}"/>
  <rect x="0" y="34" width="76" height="28" rx="6" fill="{INK}"/>
  <circle cx="14" cy="14" r="3.5" fill="{OLIVE}"/><circle cx="26" cy="14" r="3.5" fill="{AMBER}"/>
  <rect x="42" y="11" width="26" height="6" rx="3" fill="{BROWN}"/>
  <circle cx="14" cy="48" r="3.5" fill="{OLIVE}"/><circle cx="26" cy="48" r="3.5" fill="{OLIVE}"/>
  <rect x="42" y="45" width="26" height="6" rx="3" fill="{BROWN}"/>''')

part("home", 76, 62, "Home automation (a generic house, not a product logo)", f'''
  <path d="M 4 30 L 38 3 L 72 30" fill="none" stroke="{OLIVE_D}" stroke-width="7"
        stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M 13 28 V 58 H 63 V 28 L 38 8 Z" fill="{OLIVE}" stroke="{OLIVE_D}" stroke-width="3"
        stroke-linejoin="round"/>
  <rect x="31" y="38" width="14" height="20" rx="2" fill="{CREAM}"/>
  <circle cx="38" cy="25" r="5" fill="{AMBER}"/>''')

part("sd-card", 36, 40, "microSD card", f'''
  <path d="M 6 0 H 36 V 40 H 0 V 6 Z" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>
  <rect x="6" y="10" width="4" height="12" fill="{PAPER_D}"/><rect x="14" y="10" width="4" height="12" fill="{PAPER_D}"/>
  <rect x="22" y="10" width="4" height="12" fill="{PAPER_D}"/>
  <text x="18" y="34" text-anchor="middle" font-size="9" font-weight="600" fill="#7a6a52">SD</text>''')

part("board-core", 230, 170, "Analog Redial board (one line, options on the right)", f'''
  <rect id="pcb" x="0" y="0" width="230" height="170" rx="10" fill="{INK}"/>
  <rect id="jack-line-1" x="-7" y="72" width="10" height="26" rx="2" fill="{BROWN}"/>
  <rect id="options-connector" x="227" y="72" width="10" height="26" rx="2" fill="{BROWN}"/>
  <rect id="adapter-header" x="102" y="-8" width="26" height="12" rx="2" fill="{BROWN}"/>
  <circle id="led-1" cx="30" cy="18" r="4" fill="{AMBER}"/><circle id="led-2" cx="46" cy="18" r="4" fill="{OLIVE}"/>
  <text x="115" y="94" text-anchor="middle" font-size="17" font-weight="600" fill="{CREAM}">ANALOG REDIAL</text>
  <text x="115" y="116" text-anchor="middle" font-size="12" fill="{TAN}">Wi-Fi built in</text>''')

part("bluetooth-adapter", 150, 54, "Bluetooth adapter board (BM64)", f'''
  <rect id="pcb" x="0" y="0" width="150" height="44" rx="8" fill="{AMBER_D}"/>
  <rect id="chip" x="10" y="10" width="40" height="24" rx="3" fill="#6f4a14"/>
  <text x="30" y="26" text-anchor="middle" font-size="10" font-weight="600" fill="#f5e4bf">BM64</text>
  <text x="100" y="26" text-anchor="middle" font-size="12" font-weight="600" fill="#f5e4bf">Bluetooth board</text>
  <rect id="plug" x="62" y="44" width="26" height="10" rx="2" fill="{BROWN}"/>''')

part("bluetooth-adapter-side", 140, 56, "Bluetooth adapter board (BM64), plugs in from the left", f'''
  <rect id="plug" x="0" y="19" width="14" height="18" rx="2" fill="{BROWN}"/>
  <rect id="pcb" x="10" y="0" width="130" height="56" rx="8" fill="{AMBER_D}"/>
  <rect id="chip" x="20" y="14" width="42" height="28" rx="3" fill="#6f4a14"/>
  <text x="41" y="32" text-anchor="middle" font-size="10" font-weight="600" fill="#f5e4bf">BM64</text>
  <text x="103" y="25" text-anchor="middle" font-size="12" font-weight="600" fill="#f5e4bf">Bluetooth</text>
  <text x="103" y="41" text-anchor="middle" font-size="12" font-weight="600" fill="#f5e4bf">board</text>''')

part("pin-outputs", 44, 48, "Your own gadgets (relay, light, buzzer) on the spare pins", f'''
  <circle id="bulb" cx="22" cy="12" r="11" fill="{AMBER}"/>
  <rect id="bulb-base" x="15" y="22" width="14" height="6" rx="2" fill="{BROWN}"/>
  <rect id="header" x="0" y="34" width="44" height="14" rx="3" fill="{INK}"/>
  <circle cx="8" cy="41" r="3" fill="{AMBER}"/><circle cx="18" cy="41" r="3" fill="{AMBER}"/>
  <circle cx="28" cy="41" r="3" fill="{AMBER}"/><circle cx="38" cy="41" r="3" fill="{AMBER}"/>
  <path d="M 22 28 V 34" stroke="{BROWN}" stroke-width="3"/>''')

def j5_header() -> str:
    """J5, 2x4, drawn at (0,22): odd pins (the four ESP32 pins) on the top
    row, even pins (line 2's chip) on the bottom row."""
    pins = "".join(f'<rect x="{cx - 3}" y="{22 + ry - 3}" width="6" height="6" fill="{AMBER}"/>'
                   for cx in (13, 27, 41, 55) for ry in (12, 28))
    return (f'<rect id="header" x="0" y="22" width="68" height="40" rx="4" fill="{INK}"/>{pins}')


part("j5-jumpers-on", 100, 62, "J5 with all four jumpers on (second phone line)",
     j5_header() + "".join(
         f'<rect x="{cx - 5}" y="27" width="10" height="30" rx="2" fill="{OLIVE_D}" stroke="{OLIVE}" stroke-width="1.5"/>'
         for cx in (13, 27, 41, 55)))

part("j5-jumpers-off-card", 100, 62, "J5 with no jumpers, card in the slot (microSD)",
     j5_header() + f'<g transform="translate(76,22) scale(0.62)">' +
     f'<path d="M 6 0 H 36 V 40 H 0 V 6 Z" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>'
     f'<rect x="6" y="10" width="4" height="12" fill="{PAPER_D}"/><rect x="14" y="10" width="4" height="12" fill="{PAPER_D}"/>'
     f'<rect x="22" y="10" width="4" height="12" fill="{PAPER_D}"/></g>')

part("j5-jumpers-off-wires", 100, 62, "J5 with no jumpers, your wires on the top row (your own gadgets)",
     j5_header() + "".join(
         f'<path d="M {cx} 34 C {cx} 14, {cx + 8} 8, {cx + 26} 4" fill="none" stroke="{col}" '
         f'stroke-width="3" stroke-linecap="round"/>'
         for cx, col in ((13, AMBER), (27, OLIVE), (41, TAN), (55, BROWN))))

part("cord", 120, 24, "Curly cord", f'<g transform="translate(0,12)">{cord(120, 4)}</g>')


def svg_doc(w: float, h: float, body: str, title: str, bg: bool = False) -> str:
    background = f'<rect id="background" width="{w}" height="{h}" fill="{CREAM}"/>\n' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" '
            f'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            f'viewBox="0 0 {w:g} {h:g}" width="{w:g}" height="{h:g}" role="img" aria-label="{title}">\n'
            f'{background}<g font-family="{FONT}">\n{body}\n</g>\n</svg>\n')


def write_parts() -> None:
    PARTS.mkdir(parents=True, exist_ok=True)
    (PARTS / "png").mkdir(exist_ok=True)
    for name, (w, h, label, body) in PARTS_DEF.items():
        # a little margin so strokes and arcs aren't clipped at the edge
        pad = 8
        doc = svg_doc(w + 2 * pad, h + 2 * pad,
                      f'<g id="{name}" inkscape:label="{label}" transform="translate({pad},{pad})">{body}</g>',
                      label)
        (PARTS / f"{name}.svg").write_text(doc)


# ---------------------------------------------------------------- composition

class Scene:
    def __init__(self, w: float, h: float):
        self.w, self.h, self.items = w, h, []

    def add(self, gid: str, label: str, inner: str, x: float = 0, y: float = 0) -> None:
        t = f' transform="translate({x:g},{y:g})"' if (x or y) else ""
        self.items.append(f'<g id="{gid}" inkscape:label="{label}"{t}>{inner}</g>')

    def place(self, gid: str, partname: str, x: float, y: float, label: str | None = None) -> None:
        w, h, plabel, body = PARTS_DEF[partname]
        self.add(gid, label or plabel, body, x, y)

    def text(self, gid: str, s: str, x: float, y: float, size: float = 14, weight: int | str = 400,
             fill: str = TEXT, anchor: str = "middle") -> None:
        s = s.replace("&", "&amp;")
        self.add(gid, f"text: {s[:40]}",
                 f'<text x="{x:g}" y="{y:g}" text-anchor="{anchor}" font-size="{size:g}" '
                 f'font-weight="{weight}" fill="{fill}">{s}</text>')

    def line(self, gid: str, label: str, x1, y1, x2, y2, color: str, dashed: bool = False, width: float = 4) -> None:
        dash = ' stroke-dasharray="9 8"' if dashed else ""
        self.add(gid, label, f'<line x1="{x1:g}" y1="{y1:g}" x2="{x2:g}" y2="{y2:g}" stroke="{color}" '
                             f'stroke-width="{width:g}" stroke-linecap="round"{dash}/>')

    def pill(self, gid: str, s: str, cx: float, cy: float, color: str = AMBER_D) -> None:
        w = 14 + 7.4 * len(s)
        self.add(gid, f"protocol: {s}",
                 f'<rect x="{cx - w/2:g}" y="{cy - 11:g}" width="{w:g}" height="22" rx="11" fill="{CREAM}" '
                 f'stroke="{color}" stroke-width="2"/>'
                 f'<text x="{cx:g}" y="{cy + 4.5:g}" text-anchor="middle" font-size="12" font-weight="600" '
                 f'fill="{color}">{s}</text>')

    def svg(self, title: str) -> str:
        return svg_doc(self.w, self.h, "\n".join(self.items), title, bg=True)


def compose_two_line_everything() -> Scene:
    s = Scene(840, 720)
    s.text("title", "Everything at once: two lines, Bluetooth, Wi-Fi and Home Assistant",
           420, 34, 19, 600, INK)

    # --- board and the two phones (phone dial centres line up with the jacks)
    bx, by = 250, 223
    s.place("board", "board", bx, by, "Analog Redial board (2 lines)")
    s.place("module-bm64", "bluetooth-module", bx + 170, by - 9)
    s.text("board-subtitle", "Wi-Fi  ·  Bluetooth", bx + 110, by + 150, 12, 400, TAN)

    for n, py in ((1, 206), (2, 322)):
        s.place(f"phone-line-{n}", "phone", 30, py, f"Analog phone, line {n}")
        s.add(f"cord-line-{n}", f"Curly cord, line {n}", cord(bx - 7 - 126, 4), 126, py + 54)
        s.text(f"label-line-{n}", f"Line {n}", 75, py + 100, 14, 400, BROWN_D)

    # --- Bluetooth up to the cell phone
    s.place("cell-phone", "cell-phone", 409, 52, "Your cell phone")
    s.text("label-cell-phone", "Your cell phone", 398, 112, 14, 400, BROWN_D, "end")
    s.place("cellular", "cellular", 488, 84)
    s.text("label-cellular", "cellular calls", 530, 128, 13, 600, OLIVE_D, "start")
    s.line("link-bluetooth", "Bluetooth link", bx + 190, by - 9, bx + 190, 172, AMBER, dashed=True)
    s.add("bluetooth-symbol", "Bluetooth rune",
          f'<rect x="-6" y="-4" width="28" height="32" rx="6" fill="{CREAM}"/>' + PARTS_DEF["bluetooth-symbol"][3],
          bx + 182, 184)
    s.text("label-bluetooth", "Bluetooth", bx + 214, 204, 13, 600, AMBER_D, "start")

    # --- Wi-Fi down to the network, then the three services
    bar_y = 474
    s.line("link-wifi", "Wi-Fi link", bx + 110, by + 190, bx + 110, bar_y, OLIVE)
    s.add("wifi", "Wi-Fi", f'<rect x="-6" y="-2" width="72" height="44" rx="8" fill="{CREAM}"/>'
          + PARTS_DEF["wifi"][3], bx + 80, 418)
    s.add("wifi-network", "Your Wi-Fi network",
          f'<rect x="190" y="{bar_y}" width="620" height="36" rx="18" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>'
          f'<text x="500" y="{bar_y + 23}" text-anchor="middle" font-size="14" font-weight="600" fill="{BROWN_D}">'
          f'Your Wi-Fi network</text>')

    services = [
        ("web", "browser", 290, "Web interface", "Settings, recordings, dial codes", "HTTP"),
        ("voip", "server", 500, "VoIP server", "Call other extensions", "SIP + RTP"),
        ("home", "home", 710, "Home Assistant", "Events, buttons and sensors", "MQTT"),
    ]
    top = bar_y + 36 + 40
    for key, partname, cx, name, desc, proto in services:
        w, h, _, _ = PARTS_DEF[partname]
        s.line(f"link-{key}", f"Link to {name}", cx, bar_y + 36, cx, top, OLIVE)
        s.pill(f"protocol-{key}", proto, cx, bar_y + 36 + 20)
        s.place(f"service-{key}", partname, cx - w / 2, top, name)
        s.text(f"label-{key}", name, cx, top + h + 24, 15, 600, INK)
        s.text(f"desc-{key}", desc, cx, top + h + 44, 12, 400, TEXT)

    s.text("caption-1", "No microSD card and no spare pins in this build: that's what makes room for the second line.",
           420, 690, 13, 400, TEXT)
    s.text("caption-2", "Everything past the board is optional. Each link works on its own.",
           420, 708, 13, 400, TEXT)
    return s


# ---------------------------------------------------------------- overview (three sizes)

# The four spare ESP32 pins do one job at a time; the J5 jumpers pick it.
# (key, part, scale in the side layout's cards, title, what it is, what you do)
OPTIONS_SIDE = [
    ("line-2", "phone", 0.62, "Second phone line", "a second analog phone", "Jumpers on"),
    ("microsd", "sd-card", 1.0, "microSD card", "recordings and clips", "Jumpers off, card in"),
    ("pins", "pin-outputs", 1.0, "Your own gadgets", "relay, light, buzzer", "Jumpers off, your wires"),
]

DW, DH = 720, 778    # design size of the diagram itself, before any scaling

PILL_COLORS = {"ALWAYS": OLIVE_D, "PICK ONE": OLIVE_D, "BUILT-IN WI-FI": BROWN_D, "EXTRA BOARD": AMBER_D}


def pill_markup(cx: float, cy: float, label: str, color: str) -> str:
    w = 22 + 7.3 * len(label)
    return (f'<rect x="{cx - w/2:g}" y="{cy - 11:g}" width="{w:g}" height="22" rx="11" fill="{color}"/>'
            f'<text x="{cx:g}" y="{cy + 4:g}" text-anchor="middle" font-size="11" font-weight="700" '
            f'letter-spacing="0.6" fill="{CREAM}">{label}</text>'), w


def compose_overview_content() -> Scene:
    s = Scene(DW, DH)
    s.text("title", "Everything the board can connect to", 360, 24, 21, 700, INK)
    s.text("subtitle", "Every network option works on the same board. Only Bluetooth needs an extra board.",
           360, 48, 14, 400, TEXT)

    # board in the middle of the wired row
    bx, by = 190, 292
    cx_board = bx + 115
    s.place("board", "board-core", bx, by, "Analog Redial board")
    jack_y = by + 85

    # line 1: always
    s.place("phone-line-1", "phone", 0, jack_y - 54, "Analog phone (always)")
    s.add("cord-line-1", "Curly cord, line 1", cord(bx - 7 - 96, 4), 96, jack_y)
    s.text("label-line-1", "Your phone", 45, jack_y + 52, 14, 600, INK)
    s.add("tag-always", "tag: ALWAYS", pill_markup(45, jack_y + 78, "ALWAYS", PILL_COLORS["ALWAYS"])[0])

    # pick one: three cards on the right, off the board's connector
    card_w, card_h, gap = 242, 90, 14
    cx0 = DW - card_w
    top = jack_y - (3 * card_h + 2 * gap) / 2
    spine_x = bx + 237 + 31
    s.line("link-options", "Wire to the option cards", bx + 237, jack_y, spine_x, jack_y, OLIVE)
    s.line("link-options-spine", "Spine", spine_x, top + card_h / 2, spine_x,
           top + 2 * (card_h + gap) + card_h / 2, OLIVE)
    cards = OPTIONS_SIDE
    for i, (key, partname, k, title, sub, action) in enumerate(cards):
        y = top + i * (card_h + gap)
        s.line(f"stub-{key}", f"Wire to {title}", spine_x, y + card_h / 2, cx0, y + card_h / 2, OLIVE)
        w, h, _, body = PARTS_DEF[partname]
        ix = cx0 + 12 + (60 - w * k) / 2
        iy = y + (card_h - h * k) / 2
        s.add(f"option-{key}", title,
              f'<rect width="{card_w}" height="{card_h}" rx="12" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>'
              f'<g transform="translate({12 + (60 - w * k) / 2:g},{(card_h - h * k) / 2:g}) scale({k:g})">{body}</g>'
              f'<text x="82" y="31" font-size="15" font-weight="600" fill="{INK}">{title}</text>'
              f'<text x="82" y="51" font-size="12" fill="{TEXT}">{sub}</text>'
              f'<text x="82" y="72" font-size="12" font-weight="700" fill="{OLIVE_D}">{action}</text>', cx0, y)
        if i < 2:
            s.text(f"or-{i + 1}", "or", spine_x + 62, y + card_h + gap / 2 + 4, 11, 700, OLIVE_D)
    s.add("tag-pick-one", "tag: PICK ONE",
          pill_markup(cx0 + card_w / 2, top - 70, "PICK ONE", PILL_COLORS["PICK ONE"])[0])
    s.text("pick-one-heading-1", "Four spare pins, one job at a time:", cx0 + card_w / 2, top - 34, 13, 700, INK)
    s.text("pick-one-heading-2", "the jumpers pick.", cx0 + card_w / 2, top - 16, 13, 400, TEXT)

    # Bluetooth: an extra board plugged into the header, then the cell phone
    ax = cx_board - 75
    ay = by - 8 - 54 + 4
    s.place("bluetooth-adapter", "bluetooth-adapter", ax, ay, "Bluetooth adapter board (BM64)")
    s.add("tag-extra-board", "tag: EXTRA BOARD",
          pill_markup(ax - 62, ay + 22, "EXTRA BOARD", PILL_COLORS["EXTRA BOARD"])[0])
    s.place("cell-phone", "cell-phone", cx_board - 31, 78, "Your cell phone")
    s.text("label-cell-phone", "Your cell phone", cx_board - 46, 140, 14, 400, BROWN_D, "end")
    s.place("cellular", "cellular", cx_board + 44, 108)
    s.text("label-cellular", "cellular calls", cx_board + 86, 156, 13, 600, OLIVE_D, "start")
    s.line("link-bluetooth", "Bluetooth link", cx_board, 194, cx_board, ay, AMBER, dashed=True)
    s.add("bluetooth-symbol", "Bluetooth rune",
          f'<rect x="-6" y="-4" width="28" height="32" rx="6" fill="{CREAM}"/>' + PARTS_DEF["bluetooth-symbol"][3],
          cx_board - 8, 204)
    s.text("label-bluetooth", "Bluetooth", cx_board + 22, 222, 13, 600, AMBER_D, "start")

    # Wi-Fi: built in, three services hang off it
    bar_y = 548
    s.line("link-wifi", "Wi-Fi link", cx_board, by + 170, cx_board, bar_y, OLIVE)
    s.add("wifi", "Wi-Fi", f'<rect x="-6" y="-2" width="72" height="44" rx="8" fill="{CREAM}"/>'
          + PARTS_DEF["wifi"][3], cx_board - 30, by + 170 + 8)
    s.add("wifi-network", "Your Wi-Fi network",
          f'<rect x="0" y="{bar_y}" width="{DW}" height="36" rx="18" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>'
          f'<text x="{DW / 2}" y="{bar_y + 23}" text-anchor="middle" font-size="14" font-weight="600" '
          f'fill="{BROWN_D}">Your Wi-Fi network: built in, no extra hardware</text>')
    services = [
        ("web", "browser", 120, "Web interface", ["Settings, recordings, dial codes,", "and firmware updates"], "HTTP"),
        ("voip", "server", 360, "VoIP server", ["Call other extensions"], "SIP + RTP"),
        ("home", "home", 600, "Home Assistant", ["Events, buttons and sensors"], "MQTT"),
    ]
    stop = bar_y + 36 + 40
    for key, partname, cx, name, desc, proto in services:
        w, h, _, _ = PARTS_DEF[partname]
        s.line(f"link-{key}", f"Link to {name}", cx, bar_y + 36, cx, stop, OLIVE)
        s.pill(f"protocol-{key}", proto, cx, bar_y + 56)
        s.place(f"service-{key}", partname, cx - w / 2, stop, name)
        s.text(f"label-{key}", name, cx, stop + h + 24, 15, 600, INK)
        for j, line in enumerate(desc):
            s.text(f"desc-{key}-{j + 1}", line, cx, stop + h + 44 + 17 * j, 12, 400, TEXT)
    return s


# second layout: the three wired options on top, Bluetooth out to the right

DW2, DH2 = 780, 772


def compose_overview_top_content() -> Scene:
    s = Scene(DW2, DH2)
    s.text("title", "Everything the board can connect to", DW2 / 2, 24, 21, 700, INK)
    s.text("subtitle", "Every network option works on the same board. Only Bluetooth needs an extra board.",
           DW2 / 2, 48, 14, 400, TEXT)

    # --- the three wired options, on a bar above the board (mirror of Wi-Fi below)
    opt_bar_y = 232
    options = [(key, part_, k_top, cx, title, sub, action)
               for (key, part_, _, title, sub, action), (k_top, cx)
               in zip(OPTIONS_SIDE, ((0.78, 130), (1.45, 390), (1.3, 650)))]
    icon_top, icon_h = 138, 62
    for key, partname, k, cx, title, sub, action in options:
        w, h, _, body = PARTS_DEF[partname]
        ix, iy = cx - w * k / 2, icon_top + (icon_h - h * k) / 2
        s.add(f"option-{key}", title, f'<g transform="scale({k:g})">{body}</g>', ix, iy)
        s.text(f"label-{key}", title, cx, 86, 15, 600, INK)
        s.text(f"desc-{key}", sub, cx, 104, 12, 400, TEXT)
        s.text(f"action-{key}", action, cx, 123, 12, 700, OLIVE_D)
        s.line(f"link-{key}", f"Link from {title}", cx, icon_top + icon_h + 6, cx, opt_bar_y, OLIVE)
    s.text("or-1", "or", 260, icon_top + icon_h / 2 + 5, 14, 700, OLIVE_D)
    s.text("or-2", "or", 520, icon_top + icon_h / 2 + 5, 14, 700, OLIVE_D)
    s.add("options-bar", "Wired option bar",
          f'<rect x="0" y="{opt_bar_y}" width="{DW2}" height="36" rx="18" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>'
          f'<text x="{DW2 / 2}" y="{opt_bar_y + 23}" text-anchor="middle" font-size="14" font-weight="600" '
          f'fill="{BROWN_D}">Four spare pins, one job at a time: the jumpers pick.</text>')
    s.add("tag-pick-one", "tag: PICK ONE",
          pill_markup(70, opt_bar_y + 18, "PICK ONE", PILL_COLORS["PICK ONE"])[0].replace(
              f'fill="{PILL_COLORS["PICK ONE"]}"', f'fill="{PILL_COLORS["PICK ONE"]}"'))

    # --- board, with your phone on the left and the Bluetooth board on the right
    bx, by = 190, 312
    cx_board = bx + 115
    jack_y = by + 85
    s.place("board", "board-core", bx, by, "Analog Redial board")
    s.line("link-options", "Wire to the options", cx_board, opt_bar_y + 36, cx_board, by - 8, OLIVE)

    s.place("phone-line-1", "phone", 0, jack_y - 54, "Analog phone (always)")
    s.add("cord-line-1", "Curly cord, line 1", cord(bx - 7 - 96, 4), 96, jack_y)
    s.text("label-line-1", "Your phone", 45, jack_y + 52, 14, 600, INK)
    s.add("tag-always", "tag: ALWAYS", pill_markup(45, jack_y + 78, "ALWAYS", PILL_COLORS["ALWAYS"])[0])

    ax = bx + 230 - 3
    s.place("bluetooth-adapter", "bluetooth-adapter-side", ax, jack_y - 28, "Bluetooth adapter board (BM64)")
    s.add("tag-extra-board", "tag: EXTRA BOARD",
          pill_markup(ax + 80, jack_y + 54, "EXTRA BOARD", PILL_COLORS["EXTRA BOARD"])[0])
    cell_x = 650
    s.line("link-bluetooth", "Bluetooth link", ax + 140, jack_y, cell_x - 8, jack_y, AMBER, dashed=True)
    mid = (ax + 140 + cell_x - 8) / 2
    s.add("bluetooth-symbol", "Bluetooth rune",
          f'<rect x="-6" y="-4" width="28" height="32" rx="6" fill="{CREAM}"/>' + PARTS_DEF["bluetooth-symbol"][3],
          mid - 8, jack_y - 12)
    s.text("label-bluetooth", "Bluetooth", mid, jack_y - 26, 13, 600, AMBER_D)
    s.place("cell-phone", "cell-phone", cell_x, jack_y - 56, "Your cell phone")
    s.place("cellular", "cellular", cell_x + 78, jack_y - 40)
    s.text("label-cell-phone", "Your cell phone", cell_x + 31, jack_y + 76, 14, 600, INK)
    s.text("label-cellular", "cellular calls", cell_x + 31, jack_y + 95, 13, 600, OLIVE_D)

    # --- Wi-Fi down to the network and its three services
    bar_y = by + 170 + 70
    s.line("link-wifi", "Wi-Fi link", cx_board, by + 170, cx_board, bar_y, OLIVE)
    s.add("wifi", "Wi-Fi", f'<rect x="-6" y="-2" width="72" height="44" rx="8" fill="{CREAM}"/>'
          + PARTS_DEF["wifi"][3], cx_board - 30, by + 170 + 14)
    s.add("wifi-network", "Your Wi-Fi network",
          f'<rect x="0" y="{bar_y}" width="{DW2}" height="36" rx="18" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>'
          f'<text x="{DW2 / 2}" y="{bar_y + 23}" text-anchor="middle" font-size="14" font-weight="600" '
          f'fill="{BROWN_D}">Your Wi-Fi network: built in, no extra hardware</text>')
    services = [
        ("web", "browser", 130, "Web interface", ["Settings, recordings, dial codes,", "and firmware updates"], "HTTP"),
        ("voip", "server", 390, "VoIP server", ["Call other extensions"], "SIP + RTP"),
        ("home", "home", 650, "Home Assistant", ["Events, buttons and sensors"], "MQTT"),
    ]
    stop = bar_y + 36 + 40
    for key, partname, cx, name, desc, proto in services:
        w, h, _, _ = PARTS_DEF[partname]
        s.line(f"link-{key}", f"Link to {name}", cx, bar_y + 36, cx, stop, OLIVE)
        s.pill(f"protocol-{key}", proto, cx, bar_y + 56)
        s.place(f"service-{key}", partname, cx - w / 2, stop, name)
        s.text(f"label-{key}", name, cx, stop + h + 24, 15, 600, INK)
        for j, line in enumerate(desc):
            s.text(f"desc-{key}-{j + 1}", line, cx, stop + h + 44 + 17 * j, 12, 400, TEXT)
    return s


LEGEND = [
    ("ALWAYS", "One analog phone, wired to the board. It works with no network at all: dial tone, "
               "dial codes, audio clips and recordings."),
    ("PICK ONE", "The board has four spare pins that do one job at a time, and the J5 jumpers pick which. "
                 "Jumpers on: a second phone line. Jumpers off: a microSD card if one is in the slot, or your "
                 "own wires to a relay, light or buzzer if not."),
    ("BUILT-IN WI-FI", "The web interface (with firmware updates), VoIP calls to a SIP server and Home Assistant "
                       "over MQTT all run over the board's own Wi-Fi. Nothing to add: set them up in the web interface."),
    ("EXTRA BOARD", "Bluetooth is the only option that needs more hardware: a BM64 adapter board that plugs "
                    "into the header. Your analog phone then rings and answers your cell phone's calls."),
]


def wrap(text: str, width: int) -> list[str]:
    out, line = [], ""
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = f"{line} {word}".strip()
    return out + [line]


J5_STATES = [
    ("j5-jumpers-on", "Jumpers on", "2nd phone line"),
    ("j5-jumpers-off-card", "Jumpers off", "+ microSD card"),
    ("j5-jumpers-off-wires", "Jumpers off", "+ your wires"),
]


def j5_strip(x: float, y: float, step: float, k: float = 0.78) -> tuple[str, float]:
    """The three J5 states side by side, each with a two-line caption."""
    out = ['<g id="j5-states" inkscape:label="J5: the three jumper states">']
    for i, (name, l1, l2) in enumerate(J5_STATES):
        w, h, label, body = PARTS_DEF[name]
        gx = x + i * step
        out.append(f'<g id="{name}" inkscape:label="{label}" transform="translate({gx:g},{y:g}) scale({k:g})">'
                   f'{body}</g>')
        cy = y + h * k + 14
        out.append(f'<text x="{gx:g}" y="{cy:g}" font-size="11" font-weight="700" fill="{OLIVE_D}">{l1}</text>')
        out.append(f'<text x="{gx:g}" y="{cy + 14:g}" font-size="11" fill="{TEXT}">{l2}</text>')
    out.append("</g>")
    return "".join(out), 62 * k + 14 + 16


def legend_markup(x: float, y: float, chars: int, size: float = 13,
                  inline: bool = False, j5: bool = True) -> tuple[str, float]:
    """The key. Stacked (tag above its paragraph) for a narrow column, or
    inline (tag left, paragraph right) for a full-width band. Returns
    (markup, height)."""
    out, cy = [], y
    for tag, text in LEGEND:
        pw = 22 + 7.3 * len(tag)
        mk, _ = pill_markup(x + pw / 2, cy + 11, tag, PILL_COLORS[tag])
        out.append(f'<g id="legend-{tag.lower().replace(" ", "-")}" inkscape:label="legend: {tag}">{mk}')
        tx, ty = (x + 150, cy + 15) if inline else (x, cy + 40)
        lines = wrap(text, chars)
        for ln in lines:
            out.append(f'<text x="{tx:g}" y="{ty:g}" font-size="{size:g}" fill="{TEXT}">{ln}</text>')
            ty += size + 5
        if j5 and tag == "PICK ONE":
            mk, h = j5_strip(tx, ty + 2, 95 if not inline else 150)
            out.append(mk)
            ty += h + 6
        out.append("</g>")
        cy = max(ty, cy + 30) + (10 if inline else 14)
    return "\n".join(out), cy - y


SCALES = {   # per layout: (landscape k, portrait k)
    "side": (0.96, 0.93),
    "top": (0.885, 0.90),
}


def build_overview(kind: str, layout: str = "side") -> tuple[str, int, int]:
    if layout == "side":
        content, dw, dh = compose_overview_content(), DW, DH
    else:
        content, dw, dh = compose_overview_top_content(), DW2, DH2
    body = "\n".join(content.items)
    k_land, k_port = SCALES[layout]
    legend = ""
    if kind == "docs":
        m = 36
        W, H = dw + 2 * m, dh + 2 * m - 10
        k, ox, oy = 1.0, m, m - 10
    elif kind == "letter-landscape":      # 11 x 8.5 in at 96 dpi
        W, H = 1056, 816
        k = k_land
        ox, oy = 28, (H - dh * k) / 2
        lx = ox + dw * k + 30
        _, lh = legend_markup(lx, 0, 36, 13)
        ly = min(oy + 26, H - 18 - lh)        # pull the key up if it would run off the page
        legend, _ = legend_markup(lx, max(18, ly), 36, 13)
    elif kind == "letter-portrait":       # 8.5 x 11 in at 96 dpi
        W, H = 816, 1056
        _, lh = legend_markup(48, 0, 74, 13, inline=True)
        k = min(k_port, (H - 28 - 20 - lh - 16) / dh)   # shrink the diagram to make room for the key
        ox, oy = (W - dw * k) / 2, 28
        legend, _ = legend_markup(48, oy + dh * k + 20, 74, 13, inline=True)
    else:
        raise ValueError(kind)
    diagram = (f'<g id="diagram" inkscape:label="Diagram" '
               f'transform="translate({ox:g},{oy:g}) scale({k:g})">\n{body}\n</g>')
    if legend:
        diagram += f'\n<g id="legend" inkscape:label="Legend">\n{legend}\n</g>'
    doc = svg_doc(W, H, diagram,
                  "Everything the Analog Redial board can connect to: one phone always, then a second line or a "
                  "microSD card or your own gadgets on four spare pins, chosen with the J5 jumpers; Wi-Fi to the web interface, a VoIP server and Home "
                  "Assistant; and Bluetooth through an extra adapter board", bg=True)
    return doc, W, H


def export_png(svg: Path, png: Path) -> None:
    exe = shutil.which("rsvg-convert")
    if not exe:
        print("  (no rsvg-convert: skipped", png.name + ")")
        return
    subprocess.run([exe, "-z", "2", str(svg), "-o", str(png)], check=True)


def main() -> None:
    write_parts()
    for p in sorted(PARTS.glob("*.svg")):
        export_png(p, PARTS / "png" / (p.stem + ".png"))
    scene = compose_two_line_everything()
    out = ASSETS / "setup-two-line-everything.svg"
    out.write_text(scene.svg("Two phone lines on an Analog Redial board, with Bluetooth to a cell phone, "
                             "and Wi-Fi to the web interface, a VoIP server and Home Assistant"))
    export_png(out, out.with_suffix(".png"))
    for layout, prefix in (("side", "setup-overview"), ("top", "setup-overview-top")):
        for kind in ("docs", "letter-landscape", "letter-portrait"):
            doc, W, H = build_overview(kind, layout)
            path = ASSETS / f"{prefix}-{kind}.svg"
            path.write_text(doc)
            export_png(path, path.with_suffix(".png"))
            print(f"  {path.name}  {W} x {H}")
    print(f"wrote {len(PARTS_DEF)} parts and {out.name}")


if __name__ == "__main__":
    main()
