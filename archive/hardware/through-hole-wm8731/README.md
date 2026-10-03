# Through-hole / WM8731 board — archived docs

Everything specific to the original through-hole reference board
(ESP32-C6-DevKitM-1 + **WM8731** codec + AG1170 SLICs) in one place, out of
the way of the current SMD4/ES8388 docs. The board itself still builds and
runs — `CONFIG_BOARD_TYPE_THT` in `firmware/`, pin map
`board_pins.h`, codec driver `firmware/components/wm8731/` — this is just
its *documentation*, consolidated here on 2026-10-04 (was three files
scattered through `archive/hardware/`).

| File | What it is |
|------|------------|
| [`WM8731.md`](WM8731.md) | Pin-by-pin reference for the WM8731 codec itself |
| [`BOM-through-hole.md`](BOM-through-hole.md) | Parts list for the through-hole board |
| [`BRINGUP.md`](BRINGUP.md) | Hand-typed console bring-up walkthrough, including the WM8731-only commands (`wm8731_init`, `wm8731_dump`, `wm8731_bypass`) |
| [`SIGNAL_LEVELS.md`](SIGNAL_LEVELS.md) | Power/logic/audio level reference (WM8731 + AG1170) |

KiCad project: `hardware/kicad/Old/rotary_phone_gateway/`. Design doc for the
board as a whole (schematic-level, not just the codec): `../smd2.md`'s
"THT reference board" column, and `../BOM.md`, the index that still covers
both board lineages.
