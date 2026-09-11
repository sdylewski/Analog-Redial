#!/usr/bin/env python3
"""Regenerates firmware/components/webui/favicon.ico from favicon.svg.

The web UI serves two favicon formats (see webui.c's favicon_handler() and
its <link rel="icon"> tags): the SVG as-is for browsers that support scalable
vector favicons, and this .ico as a fallback for the ones that don't handle
SVG at the well-known /favicon.ico path (notably Safari, historically).
favicon.ico isn't hand-edited -- it's just multiple raster renders of the
same SVG, PNG-in-ICO (the modern container format; no separate legacy BMP
frames needed since every OS this project targets, and every browser, reads
PNG-in-ICO fine). Re-run this after editing favicon.svg (e.g. a color
change) rather than leaving the two out of sync.

Requires rsvg-convert (`brew install librsvg` / `apt install librsvg2-bin`);
everything else is stdlib. Usage: firmware/bringup/gen_favicon.py
"""

from __future__ import annotations
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SIZES = (16, 32, 48)
HERE = Path(__file__).resolve().parent
SVG_PATH = HERE.parent / "components" / "webui" / "favicon.svg"
ICO_PATH = HERE.parent / "components" / "webui" / "favicon.ico"


def render_png(svg_path: Path, size: int, out_path: Path) -> None:
    subprocess.run(
        ["rsvg-convert", "-w", str(size), "-h", str(size), str(svg_path), "-o", str(out_path)],
        check=True,
    )


def make_ico(png_paths: list[Path], out_path: Path) -> None:
    images = []
    for p in png_paths:
        data = p.read_bytes()
        # PNG IHDR: 8-byte signature, 4-byte length, 4-byte "IHDR", then
        # 4-byte big-endian width, 4-byte big-endian height.
        w = struct.unpack(">I", data[16:20])[0]
        h = struct.unpack(">I", data[20:24])[0]
        images.append((w if w < 256 else 0, h if h < 256 else 0, data))

    n = len(images)
    header = struct.pack("<HHH", 0, 1, n)
    dir_entries = b""
    image_data = b""
    offset = 6 + 16 * n
    for w, h, data in images:
        dir_entries += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(data), offset)
        image_data += data
        offset += len(data)

    out_path.write_bytes(header + dir_entries + image_data)


def main() -> int:
    if shutil.which("rsvg-convert") is None:
        print("error: rsvg-convert not found (brew install librsvg)", file=sys.stderr)
        return 1
    if not SVG_PATH.exists():
        print(f"error: {SVG_PATH} not found", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        png_paths = []
        for size in SIZES:
            p = tmp_path / f"favicon-{size}.png"
            render_png(SVG_PATH, size, p)
            png_paths.append(p)
        make_ico(png_paths, ICO_PATH)

    print(f"wrote {ICO_PATH} ({ICO_PATH.stat().st_size} bytes, {len(SIZES)} sizes: {SIZES})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
