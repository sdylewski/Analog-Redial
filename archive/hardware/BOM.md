# Bill of materials — ESP32 rotary phone

Parts lists for each board. Pick the one that matches yours.

Each line-item BOM uses the same columns: leading empty cell (your checkbox), **Qty**, **Component**, **DigiKey** (link + price in one cell), **AliExpress** (link + price), **Notes**.


| Sub-project | Board | KiCad project | BOM |
|-------------|-------|---------------|-----|
| **SMD4** (current) | ESP32-C6-DevKitC-1 + **ES8388** + **AG1171** ×1–2 | [`hardware/kicad/Analog Redial v4/`](../../hardware/kicad/Analog%20Redial%20v4/) | [**docs/bom.md**](../../docs/bom.md) |
| **SMD5** (at the fab) | as SMD4, plus BM64 Bluetooth carrier | [`hardware/kicad/Analog Redial v5/`](../../hardware/kicad/Analog%20Redial%20v5/) | [**docs/bom.md**](../../docs/bom.md) + [**BOM-bluetooth.md**](BOM-bluetooth.md) |
| **SMD2** (3 built) | ESP32-C6-WROOM-1 + **ES8388** + **AG1171** | [`hardware/kicad/Analog Redial v2/`](../../hardware/kicad/Analog%20Redial%20v2/) | [**smd2.md**](smd2.md) |
| **Through-hole / hand-solder** (reference) | ESP32-C6-DevKitM-1 + **WM8731** + **AG1170 or AG1171** | [`hardware/kicad/Old/Analog Redial v1/`](../../hardware/kicad/Old/Analog%20Redial%20v1/) | [**BOM-through-hole.md**](through-hole-wm8731/BOM-through-hole.md) |

**Design references:** [`smd2.md`](smd2.md) (SMD2, archived) · [`WM8731.md`](through-hole-wm8731/WM8731.md) (through-hole codec wiring)

**Firmware board types:** see [`../ESP32_PORT_PLAN.md`](../ESP32_PORT_PLAN.md) — one codebase, Kconfig switch for SMD vs through-hole pin map and codec driver.

---

## Build options (SMD only)

| Build | Phone lines | microSD | Firmware |
|-------|-------------|---------|----------|
| **A — Dual line** | 2× RJ12 | No | `CONFIG_BOARD_NUM_LINES=2`, `CONFIG_BOARD_MICROSD=0` |
| **B — Single line + SD** | 1× RJ12 | Yes | `CONFIG_BOARD_NUM_LINES=1`, `CONFIG_BOARD_MICROSD=1` |

Never populate **U6/J7** and **Card1** together (GPIO 18–21 XOR).

Through-hole gateway is always **2 lines**, no microSD on PCB.

---

## Assembly at a glance

| | **SMD** | **Through-hole** |
|--|---------|------------------|
| Passives | JLCPCB SMT (0402/0603/0805) | Axial / radial THT — you solder all |
| MCU | Hand: ESP32-C6-**WROOM-1** module | Hand: ESP32-C6-**DevKitM-1** (header) |
| Codec | **ES8388** — JLC or hand | **WM8731** SSOP-28 — hand |
| SLIC | **AG1171** ×1–2 — hand | **AG1170-S5** or **AG1171** ×2 — hand |
| Phone jacks | RJ12 THT — hand | RJ12 THT — hand |

---

## Revision

| Date | Notes |
|------|-------|
| 2026-06 | Split into `BOM-smd.md` and `BOM-through-hole.md`; this file is the index |
| 2026-10 | `BOM-smd.md` merged into [`smd2.md`](smd2.md) (archived whole-board reference, only 3 ever built) |
