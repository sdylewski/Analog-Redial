#!/usr/bin/env python3
"""Embed missing Device:R (and Diode base) into schematic lib_symbols cache."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCH = ROOT / "Rotary_phone_SMD.kicad_sch"
KICAD_SYM = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")


def extract_symbol(lib_text: str, symbol_name: str) -> str:
    needle = f'(symbol "{symbol_name}"'
    start = lib_text.find(needle)
    if start == -1:
        raise ValueError(f"Symbol {symbol_name!r} not found")
    depth = 0
    for i in range(start, len(lib_text)):
        ch = lib_text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return lib_text[start : i + 1]
    raise ValueError(f"Unbalanced sexpr for {symbol_name!r}")


def rename_symbol(block: str, old: str, new: str) -> str:
    return block.replace(f'(symbol "{old}"', f'(symbol "{new}"', 1)


def indent(block: str, level: int = 2) -> str:
    pad = "\t" * level
    return "\n".join(pad + line if line.strip() else line for line in block.splitlines())


def has_embedded(text: str, name: str) -> bool:
    return bool(re.search(rf'\(symbol "{re.escape(name)}"', text))


def main() -> None:
    text = SCH.read_text()
    device = (KICAD_SYM / "Device.kicad_sym").read_text()
    diode = (KICAD_SYM / "Diode.kicad_sym").read_text()

    to_embed: list[str] = []
    if not has_embedded(text, "Device:R"):
        to_embed.append(rename_symbol(extract_symbol(device, "R"), "R", "Device:R"))

    if not has_embedded(text, "Diode:1N4001"):
        to_embed.append(rename_symbol(extract_symbol(diode, "1N4001"), "1N4001", "Diode:1N4001"))

    if not has_embedded(text, "Diode:1N4148W"):
        block = extract_symbol(diode, "1N4148W")
        block = rename_symbol(block, "1N4148W", "Diode:1N4148W")
        block = block.replace('(extends "1N4001")', '(extends "Diode:1N4001")')
        to_embed.append(block)

    if not to_embed:
        print("All symbols already embedded")
        return

    marker = '\t\t(symbol "Device:C"'
    idx = text.find(marker)
    if idx == -1:
        raise SystemExit("Could not find lib_symbols insertion point")

    insertion = "".join(indent(b) + "\n" for b in to_embed)
    text = text[:idx] + insertion + text[idx:]
    SCH.write_text(text)
    print(f"Embedded {len(to_embed)} symbol(s) in {SCH.name}")


if __name__ == "__main__":
    main()
