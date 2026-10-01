#!/usr/bin/env python3
"""Migrate THT passives to 0805 SMD in Rotary_phone_SMD KiCad files."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCH = ROOT / "Rotary_phone_SMD.kicad_sch"
PCB = ROOT / "Rotary_phone_SMD.kicad_pcb"

R0805 = "Resistor_SMD:R_0805_2012Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
D_SOD123 = "Diode_SMD:D_SOD-123"
D_SMB = "Diode_SMD:D_SMB"
CP_100UF = "Capacitor_SMD:CP_Elec_6.3x7.7"
CP_220UF = "Capacitor_SMD:CP_Elec_8x10.5"

ELECTROLYTIC_FP = {
    "C2": C0805,
    "C6": C0805,
    "C7": CP_100UF,
    "C8": CP_100UF,
    "C13": CP_220UF,
    "C14": CP_220UF,
    "C19": CP_220UF,
    "C20": CP_220UF,
}

GLOBAL_FP_REPLACEMENTS = {
    "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal": R0805,
    "Capacitor_THT:C_Disc_D3.0mm_W1.6mm_P2.50mm": C0805,
    "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal": D_SOD123,
    "Capacitor_THT:CP_Radial_D6.3mm_P2.50mm": CP_100UF,
    "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm": C0805,
    "PCM_SparkFun-Resistor:R_0603_1608Metric": R0805,
    "PCM_SparkFun-Capacitor:C_0603_1608Metric": C0805,
}

LIB_ID_REPLACEMENTS = {
    "PCM_SparkFun-Resistor:R_Small": "Device:R",
    "PCM_SparkFun-Resistor:R": "Device:R",
    "PCM_SparkFun-Capacitor:C_Small": "Device:C",
    "Diode:1N4004": "Diode:1N4148W",
}


def split_blocks(text: str, opener: str) -> list[str]:
    """Split s-expression file into top-level-ish blocks starting with opener."""
    blocks: list[str] = []
    i = 0
    needle = f"({opener}"
    while i < len(text):
        start = text.find(needle, i)
        if start == -1:
            blocks.append(text[i:])
            break
        if start > i:
            blocks.append(text[i:start])
        depth = 0
        j = start
        while j < len(text):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    blocks.append(text[start : j + 1])
                    i = j + 1
                    break
            j += 1
        else:
            blocks.append(text[start:])
            break
    return blocks


def block_reference(block: str) -> str | None:
    m = re.search(r'\(property "Reference" "([^"]+)"', block)
    return m.group(1) if m else None


def set_schematic_footprint(block: str, fp: str) -> str:
    return re.sub(
        r'\(property "Footprint" "[^"]*"',
        f'(property "Footprint" "{fp}"',
        block,
        count=1,
    )


def set_pcb_footprint(block: str, fp: str) -> str:
    return re.sub(
        r'^\(footprint "[^"]*"',
        f'(footprint "{fp}"',
        block,
        count=1,
        flags=re.MULTILINE,
    )


def patch_by_reference(text: str, is_pcb: bool) -> str:
    opener = "footprint" if is_pcb else "symbol"
    parts = split_blocks(text, opener)
    out: list[str] = []
    for part in parts:
        if part.startswith(f"({opener}") and opener in ("symbol", "footprint"):
            ref = block_reference(part)
            if ref in ELECTROLYTIC_FP:
                fp = ELECTROLYTIC_FP[ref]
                part = set_pcb_footprint(part, fp) if is_pcb else set_schematic_footprint(part, fp)
        out.append(part)
    return "".join(out)


def migrate_file(path: Path, is_pcb: bool) -> None:
    text = path.read_text()

    for old, new in LIB_ID_REPLACEMENTS.items():
        text = text.replace(f'(lib_id "{old}")', f'(lib_id "{new}")')

    text = text.replace('(property "Value" "1N4004"', '(property "Value" "1N4148W"')

    for old, new in GLOBAL_FP_REPLACEMENTS.items():
        text = text.replace(old, new)

    text = text.replace(
        '(property "Value" "D_Zener P6KE82"',
        '(property "Value" "P6KE82A"',
    )

    text = patch_by_reference(text, is_pcb=is_pcb)

    if not is_pcb:
        # TVS diodes → SMB after generic diode footprint pass
        parts = split_blocks(text, "symbol")
        rebuilt: list[str] = []
        for part in parts:
            if '(lib_id "Device:D_Zener")' in part:
                part = set_schematic_footprint(part, D_SMB)
            rebuilt.append(part)
        text = "".join(rebuilt)

    path.write_text(text)
    print(f"Updated {path.name}")


def main() -> None:
    migrate_file(SCH, is_pcb=False)
    migrate_file(PCB, is_pcb=True)


if __name__ == "__main__":
    main()
