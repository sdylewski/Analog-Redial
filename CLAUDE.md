# ESP32-C6 Rotary Phone Gateway

Networked rotary telephone on custom hardware: **ESP32-C6**, stereo **I²S codec**, **Silvertel SLIC(s)** for real tip/ring phone lines. Firmware on **ESP-IDF** (hook detect, pulse/DTMF, tones, ringing; VoIP in progress). Successor to the Raspberry Pi stack in `pi_rotary_phone` (Python v1.0).

## Two hardware sub-projects

| | SMD (production) | Through-hole (reference) |
|--|--|--|
| Board | `Rotary_phone_SMD2` | `rotary_phone_gateway` |
| MCU | ESP32-C6-WROOM-1 | ESP32-C6-DevKitM-1 |
| Codec | ES8388 @ I²C 0x10 | WM8731 @ I²C 0x1A |
| SLIC | AG1171 ×1–2 | AG1170 ×2 |
| Pin map | `board_pins_smd.h` | `board_pins.h` |

One firmware tree; board type selects pin map + codec driver.

## Key docs (read before making changes)

- [`README.md`](README.md) — project overview, build/flash instructions
- [`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md) — Pi v1.0 → ESP32 scope, phases, dual-board plan
- [`FEATURES.md`](FEATURES.md) — feature port matrix, firmware architecture (components, threading model, VoIP seams)
- [`HARDWARE_SIGNAL_LEVELS.md`](HARDWARE_SIGNAL_LEVELS.md) — SLIC levels, SHK dividers
- [`MCU_VS_PI_ZERO_STACK.md`](MCU_VS_PI_ZERO_STACK.md) — MCU vs. Pi Zero tradeoffs
- [`PROJECT_UPDATE_PLAN.md`](PROJECT_UPDATE_PLAN.md) — project update plan
- [`firmware/README.md`](firmware/README.md) — ESP-IDF toolchain setup, build/flash
- Dial plan source of truth: `pi_rotary_phone/docs/DIAL_CODES.md` (sibling repo)

## Firmware architecture (summary)

ESP-IDF (FreeRTOS) C, split into components so telephony logic never touches registers directly and VoIP can be added without touching the state machine: `board` → `wm8731`/`ag1170` (hardware) → `audio`/`tones`/`dtmf` → `telephony` (per-line FSM, dial collector, dialplan) → `voip` (stub API, SIP/RTP later). See `FEATURES.md` §3 for the full diagram.

## SMD phone-line (tip/ring) PCB clearance rules

Applies to: `hardware/kicad/Rotary_phone_SMD2/**`, `hardware/ROTARY_PHONE_SMD.md`.

Subscriber-loop copper on `Rotary_phone_SMD2` can see **~48 V DC** off-hook feed, **~60 Vrms** ringing (**72 V** SLIC battery during ring per AG1171), and **~90 V** peaks (matches **P6KE82** TVS). Design spacing for **100 V peak** tip–ring.

**Clearances (2-layer, external copper, solder mask — IPC-2221 B2 baseline):**

| Between | Minimum | Project target |
|---------|---------|----------------|
| Tip ↔ ring (same line) | 0.6 mm (IPC 51–100 V external) | **0.8 mm** |
| Phone line ↔ GND / +3.3 V / +5 V / logic | 0.6 mm | **0.8 mm** |
| Line 1 phone copper ↔ Line 2 phone copper | 0.6 mm | **1.0 mm** if parallel runs are long |

Global fab minimum stays **0.15 mm**; phone-line spacing comes from the `PhoneLine` net class and `Rotary_phone_SMD2.kicad_dru`.

**KiCad setup:**
- Net class `PhoneLine` — clearance 0.8 mm, default track 0.4 mm (1.0 mm at RJ12/SLIC if space allows).
- Nets (current auto-names — re-annotate to `Line1_Tip` etc. when convenient):
  - Line 1 (U5/J6): `Net-(D5-K)` tip, `Net-(D10-A)` ring, `Net-(D10-K)` bridge/TVS node
  - Line 2 (U6/J7): `Net-(D11-A)` tip, `Net-(D12-A)` ring, `Net-(D11-K)` bridge/TVS node
- DRC: `hardware/kicad/Rotary_phone_SMD2/Rotary_phone_SMD2.kicad_dru`

**Layout:**
- Keep tip/ring paths short and clustered (RJ12 → M7 bridge → P6KE82 → AG1171 only).
- Do not route phone-line copper under the ESP32, codec, or USB.
- Prefer same-layer pairs; avoid tip/ring vias. If unavoidable, no via between paired traces.
- At tight RJ12 pads, fan out immediately — do not run tip/ring parallel closer than 0.8 mm.
- Solder mask is not safety insulation; do not shrink spacing below IPC values to save space.

This is engineering guidance, not telecom safety certification.

## Status

- Hardware: SMD2 layout in progress; through-hole gateway is the current firmware bring-up target.
- Firmware: core telephony on THT + WM8731; SMD + ES8388 + unified board Kconfig planned.
- Pi parity: align dial plan and pulse FSM with `pi_rotary_phone` v1.0 before VoIP milestone.
