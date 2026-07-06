# Bill of materials — ESP32 rotary phone

Two hardware sub-projects share one firmware tree. Pick the BOM that matches your board.

Each line-item BOM uses the same columns: leading empty cell (your checkbox), **Qty**, **Component**, **DigiKey** (link + price in one cell), **AliExpress** (link + price), **Notes**.

**DigiKey cart export:** [`2026-07-02T230626.csv`](2026-07-02T230626.csv) — source for part numbers and unit pricing where updated.

| Sub-project | Board | KiCad project | BOM |
|-------------|-------|---------------|-----|
| **SMD** (production) | ESP32-C6-WROOM-1 + **ES8388** + **AG1171** | [`kicad/Rotary_phone_SMD2/`](kicad/Rotary_phone_SMD2/) | [**BOM-smd.md**](BOM-smd.md) |
| **Through-hole / hand-solder** (reference) | ESP32-C6-DevKitM-1 + **WM8731** + **AG1170 or AG1171** | [`kicad/rotary_phone_gateway/`](kicad/rotary_phone_gateway/) | [**BOM-through-hole.md**](BOM-through-hole.md) |

**Design references:** [`ROTARY_PHONE_SMD.md`](ROTARY_PHONE_SMD.md) (SMD) · [`WM8731.md`](WM8731.md) (through-hole codec wiring)

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
