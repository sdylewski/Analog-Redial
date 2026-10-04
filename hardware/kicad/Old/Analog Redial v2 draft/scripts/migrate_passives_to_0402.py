#!/usr/bin/env python3
"""Set JLC-friendly 0402 passives; 0603/0805 only for larger ceramics."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCH = ROOT / "Rotary_phone_SMD.kicad_sch"
PCB = ROOT / "Rotary_phone_SMD.kicad_pcb"

R0402 = "Resistor_SMD:R_0402_1005Metric"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
CP_100UF = "Capacitor_SMD:CP_Elec_6.3x7.7"
CP_220UF = "Capacitor_SMD:CP_Elec_8x10.5"

# Polarized / special — by reference
REF_FOOTPRINT = {
    "C7": CP_100UF,
    "C8": CP_100UF,
    "C13": CP_220UF,
    "C14": CP_220UF,
    "C19": CP_220UF,
    "C20": CP_220UF,
}


def split_blocks(text: str, opener: str) -> list[str]:
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


def block_property(block: str, name: str) -> str | None:
    m = re.search(rf'\(property "{name}" "([^"]*)"', block)
    return m.group(1) if m else None


def set_schematic_footprint(block: str, fp: str) -> str:
    if '(property "Footprint" "' not in block:
        return block
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


def ceramic_footprint(value: str) -> str:
    v = value.lower().replace(" ", "")
    if "22uf" in v or "22µf" in v:
        return C0805
    if "10uf" in v or "10µf" in v:
        return C0603
    return C0402


def passive_footprint(ref: str | None, value: str | None, is_resistor: bool) -> str | None:
    if ref in REF_FOOTPRINT:
        return REF_FOOTPRINT[ref]
    if ref and ref.startswith("R"):
        return R0402
    if ref and ref.startswith("C") and value:
        if "CP_Elec" in value:
            return None
        return ceramic_footprint(value)
    return None


def patch_passive_blocks(text: str, is_pcb: bool) -> str:
    opener = "footprint" if is_pcb else "symbol"
    parts = split_blocks(text, opener)
    out: list[str] = []
    for part in parts:
        if not part.startswith(f"({opener}"):
            out.append(part)
            continue
        ref = block_property(part, "Reference")
        value = block_property(part, "Value")
        fp = passive_footprint(ref, value, ref.startswith("R") if ref else False)
        if fp:
            part = set_pcb_footprint(part, fp) if is_pcb else set_schematic_footprint(part, fp)
        out.append(part)
    return "".join(out)


def migrate_file(path: Path, is_pcb: bool) -> None:
    text = path.read_text()

    # Normalize any prior 0805/0603 defaults to new policy via block pass
    for old in (
        "Resistor_SMD:R_0805_2012Metric",
        "Resistor_SMD:R_0603_1608Metric",
        "Capacitor_SMD:C_0805_2012Metric",
        "Capacitor_SMD:C_0603_1608Metric",
        "PCM_SparkFun-Resistor:R_0805_2012Metric",
        "PCM_SparkFun-Capacitor:C_0805_2012Metric",
    ):
        text = text.replace(old, C0402 if "Capacitor" in old else R0402)

    text = patch_passive_blocks(text, is_pcb=is_pcb)
    path.write_text(text)
    print(f"Updated {path.name}")


def main() -> None:
    migrate_file(SCH, is_pcb=False)
    migrate_file(PCB, is_pcb=True)


if __name__ == "__main__":
    main()
