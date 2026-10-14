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
  <path d="M 0 0 h 36 v 40 h -36 v -34 l 6 -6 z" fill="{PAPER}" stroke="{PAPER_D}" stroke-width="2"/>
  <rect x="6" y="10" width="4" height="12" fill="{PAPER_D}"/><rect x="14" y="10" width="4" height="12" fill="{PAPER_D}"/>
  <rect x="22" y="10" width="4" height="12" fill="{PAPER_D}"/>
  <text x="18" y="34" text-anchor="middle" font-size="9" font-weight="600" fill="#7a6a52">SD</text>''')

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
    print(f"wrote {len(PARTS_DEF)} parts and {out.name}")


if __name__ == "__main__":
    main()
