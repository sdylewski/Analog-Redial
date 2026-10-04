# Through-hole / WM8731 board — archived docs

Everything specific to the original through-hole reference board
(ESP32-C6-DevKitM-1 + **WM8731** codec + AG1170 SLICs) in one place, out of
the way of the current SMD4/ES8388 docs.

**Firmware support was removed on 2026-10-03.** The last commit that still
builds this board is `2fdde5e` (`CONFIG_BOARD_TYPE_THT`, pin map
`components/board/include/board_pins_tht.h`, codec driver
`components/wm8731/`). To rebuild it: `git checkout 2fdde5e -- firmware`
in a scratch branch, or `git show 2fdde5e:<path>` for individual files.

| File | What it is |
|------|------------|
| [`WM8731.md`](WM8731.md) | Pin-by-pin reference for the WM8731 codec itself |
| [`BOM-through-hole.md`](BOM-through-hole.md) | Parts list for the through-hole board |
| [`BRINGUP.md`](BRINGUP.md) | Hand-typed console bring-up walkthrough, including the WM8731-only commands (`wm8731_init`, `wm8731_dump`, `wm8731_bypass`) |
| [`SIGNAL_LEVELS.md`](SIGNAL_LEVELS.md) | Power/logic/audio level reference (WM8731 + AG1170) |

KiCad project: `hardware/kicad/Old/Analog Redial v1/`. Parts-list index for
all boards: [`../BOM.md`](../BOM.md).
