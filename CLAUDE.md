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
| Pin map | `board_pins_smd4.h`, selected by `CONFIG_BOARD_TYPE_SMD4` |

The firmware tree also still supports **`CONFIG_BOARD_TYPE_THT`** (the
original through-hole reference board: ESP32-C6-DevKitM-1 + WM8731 + AG1170
×2, `board_pins.h`) and **`CONFIG_BOARD_TYPE_SMD2`** (the first hand-soldered
SMD spin, `board_pins_smd.h`) — both real, working Kconfig choices in this
codebase, just not part of the public-facing story since the project
standardized on SMD4. Don't remove them without asking; they're what the
earliest bring-up/bench testing ran on.

**SMD5 ("Analog Redial 5")** is an in-progress next-generation board (not
yet fabricated) — `hardware/kicad/Analog Redial 5/`, dev notes at
[`archive/hardware/ANALOG_REDIAL_SMD5.md`](archive/hardware/ANALOG_REDIAL_SMD5.md).
Headline change: a dual-compatible MCU footprint accepting either a real
Espressif ESP32-C6-DevKitC-1 or a cheaper AliExpress clone module, plus a
new header for the BM64 Bluetooth companion. **Not fabricated or tested** --
a re-route is still planned (MFB onto its own GPIO via an expanded BM64
header, EEPROM programming through J5 instead), so treat that doc's pinout
as a draft. Firmware reuses SMD4's pin map for it today.

## Building/flashing for a specific board type (SMD2 vs SMD4 vs THT)

Each board type is a separate build directory with its own `sdkconfig`, not
just a Kconfig toggle flipped in place — mixing these up has actually
corrupted the wrong `sdkconfig` in a past session (see incident below), so
follow this exactly:

| Board type | Build dir | Defaults file(s) |
|--|--|--|
| SMD4 (current, default) | `build/` | `sdkconfig.defaults` only — its `sdkconfig` is the one tracked in git |
| SMD2 | `build.smd2/` | `sdkconfig.defaults;sdkconfig.smd2.defaults` |
| THT | (no build.tht/ convention established yet — ask before inventing one) | `sdkconfig.defaults` only, `CONFIG_BOARD_TYPE_THT` |
| Any SMD board, 8 MB module | `build.<type>_8mb/` | `sdkconfig.defaults;sdkconfig.<type>.defaults;sdkconfig.8mb.defaults` (`partitions_8mb.csv`) |

`firmware/bringup/flash_board.py` picks and creates these automatically
(board type from the running firmware's `board_info`, flash size from
esptool). `sdkconfig.smd4.defaults` exists for SMD4 build dirs other than
`build/` — Kconfig's default board is THT.

**Creating or regenerating a non-default board's build directory**, always
with an explicit `SDKCONFIG` path:

```
idf.py -B build.smd2 -D SDKCONFIG="build.smd2/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.smd2.defaults" \
  set-target esp32c6
```

**Why the explicit `-D SDKCONFIG=...` is non-negotiable**: `idf.py -B
build.smd2 ...` without it can silently fall back to reading/writing the
*shared root* `firmware/sdkconfig` (the SMD4 one) instead of
`build.smd2/sdkconfig`, if no sdkconfig already exists inside `build.smd2/`
yet. This isn't hypothetical — it happened in the 2026-09-21 session and
silently flipped the tracked root `sdkconfig`'s `CONFIG_BOARD_TYPE` to SMD2,
caught only via `git status`/`git diff` on `firmware/sdkconfig` afterward
and fixed with `git checkout -- firmware/sdkconfig`. Always check
`git status firmware/sdkconfig` after any `-B build.<other-type>` command
that didn't pass an explicit `-D SDKCONFIG=`.

**`sdkconfig.<type>.defaults` files only fill gaps, they never override an
already-generated `sdkconfig`.** Editing one and re-running `idf.py build`
does *nothing* to a `build.<type>/sdkconfig` that already exists — the file
must be deleted (`rm -rf build.<type>` is simplest) and regenerated via
`set-target` for the new default to actually apply. This has bitten the
project more than once: `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` (the generic
`sdkconfig.defaults` still says 4MB) and, discovered 2026-09-21,
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` + `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
(every SMD board's only host-reachable console channel is native USB, not
UART0 — no bridge chip wires UART0 to anything; without this override a
fresh `build.smd2/sdkconfig` defaults to UART0 as the *primary*, input-
reading console channel, so boot/log output still appears fine over the
same USB-CDC port `idf.py monitor` uses, but literally zero keystrokes ever
reach the interactive REPL, with nothing in the output to explain why). The
lesson generalizes: `sdkconfig.defaults` is generic/stale in ways the
long-lived main `sdkconfig` has organically accumulated fixes for over
time, and every other board-type's defaults file needs those same fixes
repeated explicitly — don't assume a fresh variant inherits them.

**Flashing**: `idf.py -B build.smd2 -p <port> flash` (always pass `-B`,
never rely on the default `build/`). Prefer a direct USB flash over the
web UI's `/ota_upload` for firmware still being bench-tested — OTA writes
to the *other* OTA slot and only takes effect after a successful boot/
validation, so a build that doesn't come up cleanly can silently roll back
to whatever old image happens to already be sitting in that other slot
(observed 2026-09-21: reverted to a build old enough to be missing console
commands added since).

## Bench-debugging GPIOs or board connections

**Always configure a GPIO output with the full `gpio_config()` struct
(explicit `pull_up_en`/`pull_down_en`/`intr_type`), never a bare
`gpio_set_direction()` + `gpio_set_level()` pair.** The bare pair doesn't
touch `pull_up_en` at all — a leftover internal pull-up (including one from
ESP-IDF's own power-on defaults, or from a `gpio_reset_pin()` call, which
resets a pin to its default state, pull-up included) can silently fight the
output driver, and neither `gpio_set_level()`'s return value nor a
`gpio_get_level()` readback will reveal it (`GPIO_MODE_OUTPUT` leaves
`InputEn: 0` on this chip, so a software readback of an output-only pin
isn't trustworthy either way — don't trust that diagnostic). This cost most
of two evenings misdiagnosed as a wiring/connection fault while bringing up
a bench-wired BM64 module on GPIO18-21 (full story:
`archive/BM64_MFB_GPIO_DEBUG.md`) — `ag1170.c`'s `config_line_gpio()` never
hit it because it already uses full `gpio_config()`.
`gpio_dump_io_configuration(stdout, 1ULL << pin)` (`driver/gpio.h`) is the
tool that actually shows what's happening at the pad (pull state, output
enable, sleep-select, GPIO-matrix routing) — reach for it early on a GPIO
that "should" be working but measures wrong, not after hours of
wiring-focused debugging.

Separately, also worth checking early (a real thing, just not what caused
the above): Settings → Hardware → "Low-power idle mode" enables WiFi
power-save after N minutes of an idle phone line (default 5,
`components/idle_power/idle_power.c`), which permits automatic light sleep
between WiFi beacon windows — ESP-IDF's boot log line "sleep: Configure to
isolate all GPIO pins in sleep state" means GPIOs can float during those
windows unless a pin has explicitly called `gpio_sleep_sel_dis()`. A
multi-hour bench session will always run past that idle threshold. Disable
it while debugging (`curl -X POST -d "minutes=5" http://<board>/idle_power`
— the `enabled` field is a checkbox, omitting it disables) to rule it out
before chasing something else.

## Public vs. internal docs

This repo is public on GitHub. `README.md` and everything under `docs/` are
written for someone building a board from scratch — SMD4 only, no
dev-journal detail. Everything else lives under **[`archive/`](archive/)** —
dev-journal docs (`archive/ECHO_DEBUGGING.md`, `archive/ESP32_PORT_PLAN.md`,
`archive/PROJECT_UPDATE_PLAN.md`, `archive/MCU_VS_PI_ZERO_STACK.md`,
`archive/VOIP_SETUP.md`), and superseded/pre-SMD4 hardware docs
(`archive/hardware/ROTARY_PHONE_SMD*.md`, `BOM*.md`, and the through-hole/WM8731 board's own docs under `archive/hardware/through-hole-wm8731/`). These are
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
- [`docs/building.md`](docs/building.md) — board assembly: hand-solder order, pre-power checks, the SMD4 15k rework
- [`docs/hardware-smd4.md`](docs/hardware-smd4.md) — SMD4 schematic reference, GPIO pin map, power tree
- [`docs/hardware-smd5.md`](docs/hardware-smd5.md) — SMD5 board reference (Bluetooth companion, LDO, dual MCU footprint); not yet fabricated
- [`archive/signal-levels.md`](archive/signal-levels.md) — SLIC/codec levels, SHK dividers
- [`docs/voip-setup.md`](docs/voip-setup.md) — SIP server setup, dial-plan reference
- [`firmware/README.md`](firmware/README.md) — ESP-IDF toolchain setup, build/flash
- [`archive/ESP32_PORT_PLAN.md`](archive/ESP32_PORT_PLAN.md) *(internal)* — Pi v1.0 → ESP32 scope, phases, dual-board plan
- Dial plan source of truth: `pi_rotary_phone/docs/DIAL_CODES.md` (sibling repo)

## Firmware architecture (summary)

ESP-IDF (FreeRTOS) C, split into components so telephony logic never touches registers directly and VoIP can be added without touching the state machine: `board` → `wm8731`/`ag1170` (hardware — component dir names predate the SMD4/ES8388 switch, still accurate to what's actually in each dir) → `audio`/`tones`/`dtmf` → `telephony` (per-line FSM, dial collector, dialplan) → `voip` (SIP/RTP). See `docs/features.md` §3 for the full diagram.

## SMD phone-line (tip/ring) PCB clearance rules

Applies to: `hardware/kicad/Rotary_phone_SMD4/**`, `docs/hardware-smd4.md`.

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
- 2-layer board: no internal ground plane — confirm a continuous GND pour actually exists under the tip/ring path, not just traces (see `docs/hardware-smd4.md` §8).

This is engineering guidance, not telecom safety certification.

## Devin Agent Notes

- **Do NOT run `launchctl`** — `launchctl` is blocked by organization policy.
  If you need to set environment variables for GUI apps (e.g. `SSL_CERT_FILE`
  for KiCad's bundled Python), describe the commands and ask the user to run
  them in their own terminal.

## Status

- Hardware: SMD4 is the current design — schematic unchanged from SMD3, re-routed as a 2-layer PCB. ERC/DRC-reviewed; not yet fabricated/bench-tested.
- Firmware: core telephony is mature and tested on earlier SMD-family hardware (hand-soldered SMD2, ES8388 + AG1171) and on THT + WM8731, selected via board Kconfig (`CONFIG_BOARD_TYPE_THT`/`_SMD2`); runtime `codec_detect` warns on board/firmware mismatch.
- Pi parity: align dial plan and pulse FSM with `pi_rotary_phone` v1.0 before VoIP milestone.
