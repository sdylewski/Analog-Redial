# ESP32-C6 Rotary Phone Gateway

A networked **rotary telephone** on custom hardware: **ESP32-C6**, a stereo **I²S codec**, and **Silvertel SLIC(s)** for real tip/ring phone lines. Firmware runs on **ESP-IDF** (hook detect, pulse/DTMF, tones, ringing; VoIP in progress).

Successor to the Raspberry Pi stack in [`pi_rotary_phone`](../pi_rotary_phone) (Python v1.0 — see [`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md) for port scope).

> **New here? Start with [`GETTING_STARTED.md`](GETTING_STARTED.md)** — install the toolchain, configure for your board, build/flash, and set up Wi-Fi from scratch.

---

## Two hardware sub-projects

| | **SMD** (production) | **Through-hole / hand-solder** (reference) |
|--|----------------------|---------------------------------------------|
| **Board** | [`Rotary_phone_SMD2`](hardware/kicad/Rotary_phone_SMD2/) | [`rotary_phone_gateway`](hardware/kicad/rotary_phone_gateway/) |
| **MCU** | ESP32-C6-**WROOM-1** module | ESP32-C6-**DevKitM-1** |
| **Codec** | **ES8388** (QFN) | **WM8731** (SSOP) |
| **SLIC** | **AG1171** ×1–2 | **AG1170** ×2 |
| **Passives** | 0402 SMD + JLC assembly | Axial / radial THT — you solder all |
| **BOM** | [`hardware/BOM-smd.md`](hardware/BOM-smd.md) | [`hardware/BOM-through-hole.md`](hardware/BOM-through-hole.md) |
| **Design doc** | [`hardware/ROTARY_PHONE_SMD.md`](hardware/ROTARY_PHONE_SMD.md) | [`hardware/WM8731.md`](hardware/WM8731.md) |

**One firmware tree** — board type selects pin map + codec driver (planned Kconfig; today defaults to through-hole). Details: [`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md).

---

## What’s in the repo

| Path | Description |
|------|-------------|
| [`firmware/`](firmware/) | ESP-IDF application — [`firmware/README.md`](firmware/README.md) |
| [`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md) | **Pi v1.0 → ESP32 scope**, dual-board plan, phases, libraries |
| [`hardware/BOM.md`](hardware/BOM.md) | BOM index (SMD vs through-hole) |
| [`hardware/BOM-smd.md`](hardware/BOM-smd.md) | SMD board shopping checklist |
| [`hardware/BOM-through-hole.md`](hardware/BOM-through-hole.md) | Through-hole gateway checklist |
| [`hardware/kicad/Rotary_phone_SMD2/`](hardware/kicad/Rotary_phone_SMD2/) | **SMD** KiCad project |
| [`hardware/ROTARY_PHONE_SMD.md`](hardware/ROTARY_PHONE_SMD.md) | SMD design reference |
| [`hardware/kicad/rotary_phone_gateway/`](hardware/kicad/rotary_phone_gateway/) | **Through-hole** KiCad project |
| [`FEATURES.md`](FEATURES.md) | Feature matrix and firmware architecture |
| [`HARDWARE_SIGNAL_LEVELS.md`](HARDWARE_SIGNAL_LEVELS.md) | SLIC levels, SHK dividers |
| [`datasheets/`](datasheets/) | Local PDFs |

---

## Hardware overview (SMD board)

```
USB-C ──► +5 V ──┬──► AP2112K ──► +3.3 V ──► ESP32-C6-WROOM-1 (U2)
                 │                      └──► ES8388 (U4) ◄──I²S/I²C──►
                 └──► AG1171 SLIC(s) ◄──RJ12──► rotary phone(s)
```

| Block | Part | How it’s built |
|-------|------|----------------|
| MCU | **ESP32-C6-WROOM-1** | **Hand-solder** (castellated module) |
| Codec | **ES8388** | **JLCPCB assembly** (QFN) or hand hot-air |
| SLIC | **AG1171** ×1 or ×2 | **Hand-solder** (14-pin SIL) |
| Phone jacks | **RJ12** | **Hand-solder** (THT) |
| Passives, USB-C, diodes | 0402/0603/0805 SMD | **JLCPCB economic assembly** |
| LDO, USB ESD | AP2112K, USBLC6 | **Hand-solder** (mark DNP on JLC BOM) |
| VBUS Schottky **D1** | SS14 | **JLCPCB assembly** (Basic **C2480**) |

**Board size:** ~100 × 50 mm, 2-layer.  
**Power:** USB-C **5 V** or optional **J8** screw terminal; SLIC run from **+5 V** branch.

### Build variants

| Build | Lines | microSD | Use when |
|-------|-------|---------|----------|
| **A** | 2× RJ12 | No | Two phones, dual-line firmware |
| **B** | 1× RJ12 | Yes (TF socket) | Single phone + SD storage |

See [`hardware/BOM-smd.md`](hardware/BOM-smd.md) for the full stuffing checklist (build A vs B).

---

## Firmware

- **Framework:** [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.2+  
- **Target:** `esp32c6`  
- **Board config:** `firmware/components/board/include/board_pins_smd.h` (SMD) or `board_pins.h` (legacy THT)

```bash
cd firmware
idf.py set-target esp32c6
idf.py build flash monitor
```

Details, toolchain setup, and `sdkconfig` options: [`firmware/README.md`](firmware/README.md).

---

## Building the PCB

### 1. KiCad

Open `hardware/kicad/Rotary_phone_SMD2/Rotary_phone_SMD2.kicad_pro`.

### 2. JLCPCB (recommended)

1. Install **JLCPCB Tools** in KiCad PCM (add [Bouni’s repository](https://github.com/Bouni/kicad-jlcpcb-tools)).
2. Assign / verify **LCSC** numbers; set **DNP** on hand-solder parts (see [`hardware/BOM-smd.md`](hardware/BOM-smd.md)).
3. **Generate** → upload Gerber + BOM + CPL to [jlcpcb.com](https://jlcpcb.com).

### 3. Hand assembly after boards arrive

Order the hand-solder kit from DigiKey using  
[`hardware/kicad/Rotary_phone_SMD2/bom-digikey-hand-solder.csv`](hardware/kicad/Rotary_phone_SMD2/bom-digikey-hand-solder.csv)  
(trim rows for build A vs B).

Suggested order: reflow/hot-air **U2**, **U1**, **U3** → solder **AG1171**, **RJ12**, **470 µF**, **TVS** → test USB and one phone line before second SLIC (build A). **D1** SS14 is JLC-assembled.

---

## Documentation map

| Topic | Document |
|-------|----------|
| **Port plan (Pi v1.0, dual board, phases)** | [`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md) |
| SMD schematic, ES8388, SLIC | [`hardware/ROTARY_PHONE_SMD.md`](hardware/ROTARY_PHONE_SMD.md) |
| BOM checklists | [`hardware/BOM.md`](hardware/BOM.md) → SMD or through-hole |
| Telephony features & architecture | [`FEATURES.md`](FEATURES.md) |
| Electrical levels | [`HARDWARE_SIGNAL_LEVELS.md`](HARDWARE_SIGNAL_LEVELS.md) |
| Through-hole WM8731 board | [`hardware/WM8731.md`](hardware/WM8731.md) |

---

## Publishing to GitHub

The repo is initialized locally but may not have an initial commit yet. Typical first push:

```bash
cd /path/to/esp32_rotary_phone
git add README.md FEATURES.md HARDWARE_SIGNAL_LEVELS.md firmware/ hardware/ datasheets/ .gitignore
git status   # review; do not commit secrets or .env files
git commit -m "Initial open-source release: ESP32-C6 rotary phone gateway."
git branch -M main
git remote add origin https://github.com/YOUR_USER/esp32_rotary_phone.git
git push -u origin main
```

Add a **LICENSE** file before publishing if you want others to know usage terms (MIT is common for hardware + firmware hobby projects).

**Not tracked** (see `.gitignore`): KiCad `.history/`, `jlcpcb/` export folders, Python venvs, IDE metadata.

---

## Status

- **Hardware:** SMD2 boards received and being bench-tested; SMD3 layout in progress
- **Firmware:** Core telephony on THT + WM8731 and SMD2 + ES8388, selected via unified board Kconfig; runtime codec detection warns on board/firmware mismatch — [`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md)
- **Pi parity:** Align dial plan and pulse FSM with [`pi_rotary_phone`](../pi_rotary_phone) v1.0 before VoIP milestone

Contributions and issues welcome once the repo is public.
