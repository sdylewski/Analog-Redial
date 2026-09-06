# Analog Redial

An ESP32-C6 analog phone exchange: **ESP32-C6**, stereo **I²S codec** (ES8388), **Silvertel AG1171 SLIC(s)** for real tip/ring phone lines. Firmware on **ESP-IDF** (hook detect, pulse/DTMF, tones, ringing, VoIP). Successor to the Raspberry Pi stack in `pi_rotary_phone` (Python v1.0).

Public docs (README, `docs/`) present the current **SMD4** board as the only
board — see [Public vs. internal docs](#public-vs-internal-docs) below for
why, and what else still exists in this tree.

## Current hardware: SMD4

| | SMD4 (current) |
|--|--|
| Board | `hardware/kicad/Rotary_phone_SMD4/` |
| MCU | ESP32-C6-DevKitC-1 |
| Codec | ES8388 @ I²C 0x10 |
| SLIC | AG1171 ×1–2 |
| Pin map | `board_pins_smd.h` (SMD4-specific header not yet written — see `docs/hardware.md` §3; `CONFIG_BOARD_TYPE_SMD3`/`_SMD4` Kconfig choice needed alongside the existing ones) |

The firmware tree also still supports **`CONFIG_BOARD_TYPE_THT`** (the
original through-hole reference board: ESP32-C6-DevKitM-1 + WM8731 + AG1170
×2, `board_pins.h`) and **`CONFIG_BOARD_TYPE_SMD2`** (the first hand-soldered
SMD spin) — both real, working Kconfig choices in this codebase, just not
part of the public-facing story since the project standardized on SMD4.
Don't remove them without asking; they're what the earliest bring-up/bench
testing ran on.

## Public vs. internal docs

This repo is public on GitHub. `README.md` and everything under `docs/` are
written for someone building a board from scratch — SMD4 only, no
dev-journal detail. Everything else lives under **[`archive/`](archive/)** —
dev-journal docs (`archive/ECHO_DEBUGGING.md`, `archive/ESP32_PORT_PLAN.md`,
`archive/PROJECT_UPDATE_PLAN.md`, `archive/MCU_VS_PI_ZERO_STACK.md`,
`archive/VOIP_SETUP.md`), and superseded/pre-SMD4 hardware docs
(`archive/hardware/ROTARY_PHONE_SMD*.md`, `BOM*.md`, `WM8731.md`). These are
**your working notes, not part of the public doc set** — useful for you when
picking up this project again, not linked from the public README. When
adding new engineering-journal-style content (debug sessions, open
questions, measured bench data), it belongs under `archive/`, not in
`docs/`. Cross-references between files already inside `archive/` keep
working unchanged (the whole subtree moved together, same relative
structure) — only links from live files (this one included) need the
`archive/` prefix.

## Key docs (read before making changes)

- [`README.md`](README.md) — public project overview, build/flash instructions
- [`docs/features.md`](docs/features.md) — feature list, firmware architecture (components, threading model, VoIP seams)
- [`docs/hardware.md`](docs/hardware.md) — SMD4 schematic reference, GPIO pin map, power tree
- [`docs/signal-levels.md`](docs/signal-levels.md) — SLIC/codec levels, SHK dividers
- [`docs/voip-setup.md`](docs/voip-setup.md) — SIP server setup, dial-plan reference
- [`firmware/README.md`](firmware/README.md) — ESP-IDF toolchain setup, build/flash
- [`archive/ESP32_PORT_PLAN.md`](archive/ESP32_PORT_PLAN.md) *(internal)* — Pi v1.0 → ESP32 scope, phases, dual-board plan
- Dial plan source of truth: `pi_rotary_phone/docs/DIAL_CODES.md` (sibling repo)

## Firmware architecture (summary)

ESP-IDF (FreeRTOS) C, split into components so telephony logic never touches registers directly and VoIP can be added without touching the state machine: `board` → `wm8731`/`ag1170` (hardware — component dir names predate the SMD4/ES8388 switch, still accurate to what's actually in each dir) → `audio`/`tones`/`dtmf` → `telephony` (per-line FSM, dial collector, dialplan) → `voip` (SIP/RTP). See `docs/features.md` §3 for the full diagram.

## SMD phone-line (tip/ring) PCB clearance rules

Applies to: `hardware/kicad/Rotary_phone_SMD4/**`, `docs/hardware.md`.

Subscriber-loop copper on this board can see **~48 V DC** off-hook feed, **~60 Vrms** ringing (**72 V** SLIC battery during ring per AG1171), and **~90 V** peaks (matches **P6KE82**-class TVS). Design spacing for **100 V peak** tip–ring.

**Clearances (2-layer, external copper, solder mask — IPC-2221 B2 baseline):**

| Between | Minimum | Project target |
|---------|---------|----------------|
| Tip ↔ ring (same line) | 0.6 mm (IPC 51–100 V external) | **0.8 mm** |
| Phone line ↔ GND / +3.3 V / +5 V / logic | 0.6 mm | **0.8 mm** |
| Line 0 phone copper ↔ Line 1 phone copper | 0.6 mm | **1.0 mm** if parallel runs are long |

Global fab minimum stays **0.15 mm**; phone-line spacing comes from the `PhoneLine` net class and `Rotary_phone_SMD4.kicad_dru`.

**KiCad setup:**
- Net class `PhoneLine` — clearance 0.8 mm, default track 0.4 mm (1.0 mm at RJ12/SLIC if space allows).
- Nets: check the current netlist (`kicad-cli sch export netlist` or the KiCad GUI) rather than trusting a hardcoded list here — auto-generated net names (`Net-(D#-K)` style) can shift between spins/re-annotations. Re-annotate to `Line0_Tip` etc. when convenient.
- DRC: `hardware/kicad/Rotary_phone_SMD4/Rotary_phone_SMD4.kicad_dru`

**Layout:**
- Keep tip/ring paths short and clustered (RJ12 → bridge diodes → TVS → AG1171 only).
- Do not route phone-line copper under the ESP32, codec, or USB.
- Prefer same-layer pairs; avoid tip/ring vias. If unavoidable, no via between paired traces.
- At tight RJ12 pads, fan out immediately — do not run tip/ring parallel closer than 0.8 mm.
- Solder mask is not safety insulation; do not shrink spacing below IPC values to save space.
- 2-layer board: no internal ground plane — confirm a continuous GND pour actually exists under the tip/ring path, not just traces (see `docs/hardware.md` §8).

This is engineering guidance, not telecom safety certification.

## Status

- Hardware: SMD4 is the current design — schematic unchanged from SMD3, re-routed as a 2-layer PCB. ERC/DRC-reviewed; not yet fabricated/bench-tested. `board_pins_smd4.h` and its Kconfig choice still need to be written once boards are back (see `docs/hardware.md` §3).
- Firmware: core telephony is mature and tested on earlier SMD-family hardware (hand-soldered SMD2, ES8388 + AG1171) and on THT + WM8731, selected via board Kconfig (`CONFIG_BOARD_TYPE_THT`/`_SMD2`); runtime `codec_detect` warns on board/firmware mismatch.
- Pi parity: align dial plan and pulse FSM with `pi_rotary_phone` v1.0 before VoIP milestone.
