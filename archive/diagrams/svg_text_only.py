#!/usr/bin/env python3
"""Cuts the wordmark's letters out of docs/assets/cutout/analog_redial_logo_with_text.svg:
the same solid outlines, with the dial icon (the sub-paths drawn with arcs) left out
and the gap where it sat kept, so ANALOG and REDIAL stay at the same spacing.
Writes analog_redial_text_only.svg (then run svg_to_dxf.py for the DXF).

Needs svgpathtools.
"""
import re
from pathlib import Path

from svgpathtools import Arc, Line, QuadraticBezier, parse_path

CUT = Path(__file__).resolve().parents[2] / "docs" / "assets" / "cutout"
src = (CUT / "analog_redial_logo_with_text.svg").read_text()
w_mm = float(re.search(r'width="([\d.]+)mm"', src).group(1))
vb_w, vb_h = map(float, re.search(r'viewBox="[\d.\-]+ [\d.\-]+ ([\d.]+) ([\d.]+)"', src).groups())
k = w_mm / vb_w

subs = [s for s in parse_path(re.search(r' d="([^"]+)"', src).group(1)).continuous_subpaths()
        if not any(isinstance(g, Arc) for g in s)]
f = lambda z: f"{z.real:.2f} {z.imag:.2f}"
d = ""
for s in subs:
    d += f"M{f(s[0].start)}"
    for g in s:
        if isinstance(g, Line):
            d += f"L{f(g.end)}"
        elif isinstance(g, QuadraticBezier):
            d += f"Q{f(g.control)} {f(g.end)}"
        else:
            raise SystemExit(f"unexpected segment {type(g).__name__}")
    d += "Z"
(CUT / "analog_redial_text_only.svg").write_text(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{w_mm:.2f}mm" height="{vb_h * k:.2f}mm" '
    f'viewBox="0.00 0.00 {vb_w:.2f} {vb_h:.2f}"><path fill="#000000" fill-rule="nonzero" d="{d}"/></svg>\n')
print(f"{len(subs)} letter outlines kept, {w_mm} mm wide")
