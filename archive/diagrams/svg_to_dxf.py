#!/usr/bin/env python3
"""Converts the single-path cutout SVGs in docs/assets/cutout to DXF (millimetres)
for CAD import (Onshape: Insert -> DXF into a sketch).

Full circles become CIRCLE entities, outlines made only of lines and arcs (the dial's
finger stop) become LINE and ARC entities, and every other closed outline (the letters)
a closed LWPOLYLINE (curves sampled finer than 0.01 mm). The SVG's size in mm comes from
its width/height attributes; Y is flipped (SVG's origin is top-left), so the drawing sits in the +X +Y quadrant with its lower-left corner at 0,0.

  python3 archive/diagrams/svg_to_dxf.py [file.svg ...]    (default: all in docs/assets/cutout)

Needs ezdxf and svgpathtools (pip install ezdxf svgpathtools).
"""
import math
import re
import sys
from pathlib import Path

import ezdxf
from svgpathtools import Arc, Line, Path as SvgPath, parse_path

CUTOUT = Path(__file__).resolve().parents[2] / "docs" / "assets" / "cutout"


def convert(svg: Path) -> Path:
    text = svg.read_text()
    w_mm = float(re.search(r'width="([\d.]+)mm"', text).group(1))
    vb_w, vb_h = map(float, re.search(r'viewBox="[\d.\-]+ [\d.\-]+ ([\d.]+) ([\d.]+)"', text).groups())
    k = w_mm / vb_w
    d = re.search(r' d="([^"]+)"', text).group(1)

    doc = ezdxf.new("R2010", setup=True)
    doc.units = ezdxf.units.MM
    msp = doc.modelspace()
    n_circ = n_poly = n_pts = 0
    X = lambda z: z.real * k
    Y = lambda z: (vb_h - z.imag) * k
    for sub in parse_path(d).continuous_subpaths():
        # the closing "Z" adds a zero-length line when the outline already ended where it began
        sub = SvgPath(*[g for g in sub if not (isinstance(g, Line) and g.length() < 1e-6)])
        arcs = [g for g in sub if isinstance(g, Arc)]
        straight = all(isinstance(g, (Line, Arc)) for g in sub)
        if arcs and len(arcs) == len(sub) and abs(sub.start - sub.end) < 1e-6 \
                and all(abs(g.radius - arcs[0].radius) < 1e-6 and abs(g.center - arcs[0].center) < 1e-6 for g in arcs):
            c = arcs[0].center                       # one circle, drawn as two (or more) arcs
            msp.add_circle((X(c), Y(c)), arcs[0].radius.real * k)
            n_circ += 1
        elif straight:                               # lines and arcs only: keep them as exact entities
            for g in sub:
                if isinstance(g, Line):
                    msp.add_line((X(g.start), Y(g.start)), (X(g.end), Y(g.end)))
                else:
                    c = g.center
                    ang = lambda z: math.degrees(math.atan2(Y(z) - Y(c), X(z) - X(c)))
                    a0, a1 = ang(g.start), ang(g.end)
                    if g.sweep:                      # SVG sweep 1 is clockwise on screen; a DXF ARC always runs counter-clockwise
                        a0, a1 = a1, a0
                    msp.add_arc((X(c), Y(c)), g.radius.real * k, a0, a1)
            n_poly += 1
        else:
            pts = []
            for seg in sub:
                n = 1 if isinstance(seg, Line) else max(8, int(seg.length() * k / 0.2))
                pts += [seg.point(i / n) for i in range(n)]
            msp.add_lwpolyline([(X(p), Y(p)) for p in pts], close=True)
            n_poly += 1
            n_pts += len(pts)
    out = svg.with_suffix(".dxf")
    doc.saveas(out)
    print(f"{out.name}: {n_circ} circles, {n_poly} other outlines ({n_pts} polyline points), {w_mm} mm wide")
    return out


if __name__ == "__main__":
    for f in ([Path(a) for a in sys.argv[1:]] or sorted(CUTOUT.glob("*.svg"))):
        convert(f)
