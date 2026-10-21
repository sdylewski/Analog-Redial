# Analog Redial

An ESP32-C6 analog phone exchange: **ESP32-C6**, stereo **I²S codec** (ES8388), **Silvertel AG1171 SLIC(s)** for real tip/ring phone lines. Firmware on **ESP-IDF** (hook detect, pulse/DTMF, tones, ringing, VoIP). Successor to the Raspberry Pi stack in `pi_rotary_phone` (Python v1.0).

Public docs (README, `docs/`) present the current **SMD4** board as the only
board — see [Public vs. internal docs](#public-vs-internal-docs) below for
why, and what else still exists in this tree.

## Current hardware: SMD4

| | SMD4 (current) |
|--|--|
| Board | `hardware/kicad/Analog Redial v4/` |
| MCU | ESP32-C6-DevKitC-1 |
| Codec | ES8388 @ I²C 0x10 |
| SLIC | AG1171 ×1–2 |
| Pin map | `board_pins_smd4.h`, selected by `CONFIG_BOARD_TYPE_SMD4` |

The firmware tree also still supports **`CONFIG_BOARD_TYPE_SMD2`** (the
first hand-soldered SMD spin, `board_pins_smd.h`) — a real, working Kconfig
choice, just not part of the public-facing story since the project
standardized on SMD4. Don't remove it without asking.

The original through-hole board (WM8731 codec, `CONFIG_BOARD_TYPE_THT`) was
**removed from the firmware on 2026-10-03** at the user's request — driver,
pin map and Kconfig choice all gone. Last commit that still builds it:
`2fdde5e`. Its docs remain under `archive/hardware/through-hole-wm8731/`.

**SMD5 ("Analog Redial 5")** is the current-generation board — boards back from
the fab 2026-10-19; documented as working (BM64 carrier, microSD, audio, OTA verified on the first two boards; see
`archive/hardware/smd5.md`'s Bring-up log) — `hardware/kicad/Analog Redial v5/`,
dev notes at [`archive/hardware/smd5.md`](archive/hardware/smd5.md).
Headline changes: a dual-compatible MCU footprint (real ESP32-C6-DevKitC-1 or
a cheaper clone, selected by solder jumpers JP2/JP3), and an 8-pin J2 header
for a Bluetooth companion, MFB/WAKEUP on GPIO15, chosen in Settings → Hardware
(`board_bt_module_t`). Firmware reuses SMD4's pin map for it. The BM64 carrier is
the supported module; a BK3266 module was tried and its UART never answered (notes in
`Bluetooth/BK3266/` and `archive/hardware/bk3266.md`); the next candidate is the DFRobot
DFR0781 / BT401 (`Bluetooth/DFR0781_BT401/`), untested.

**⚠️ v5 PCB error with 30-pin clone modules:** the footprint ties the clone's bottom-right
5V pin to GND. That pin must be removed (not connected to the board); the bottom-left 5V pin
carries the 5V. Documented as a warning in README/docs (board.md, bom.md, hardware-smd5.md);
fixed in v6. Keep that warning in any doc that describes fitting a clone.

**v6 (SMD6, `hardware/kicad/Analog Redial v6/`) is in progress and untested.** The public
docs say **build v5 for now** (README history table, docs/board.md version table and
callout, and a README in the v6 KiCad folder). Don't describe v6 as working or
recommend ordering it until the user has tested it and says so.

## Building/flashing for a specific board type (SMD2 vs SMD4 vs SMD5)

Each board type is a separate build directory with its own `sdkconfig`, not
just a Kconfig toggle flipped in place — mixing these up has actually
corrupted the wrong `sdkconfig` in a past session (see incident below), so
follow this exactly:

| Board type | Build dir | Defaults file(s) |
|--|--|--|
| SMD4 (current, default) | `build/` | `sdkconfig.defaults` only — its `sdkconfig` is the one tracked in git |
| SMD2 | `build.smd2/` | `sdkconfig.defaults;sdkconfig.smd2.defaults` |
| Any SMD board, 8 MB module | `build.<type>_8mb/` | `sdkconfig.defaults;sdkconfig.<type>.defaults;sdkconfig.8mb.defaults` (`partitions_8mb.csv`) |

`firmware/bringup/flash_board.py` picks and creates these automatically
(board type from the running firmware's `board_info`, flash size from
esptool). `sdkconfig.smd4.defaults` exists for SMD4 build dirs other than
`build/` (it carries the 16 MB / native-USB settings `sdkconfig.defaults` lacks).

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
dev-journal docs (`archive/ECHO_DEBUGGING.md`, `archive/BLUETOOTH_AUDIO_UPGRADE.md`,
`archive/ANSWERING_MACHINE.md`, `archive/POWER_SAVING.md`, `archive/VOIP_SETUP.md`), and superseded/pre-SMD4 hardware docs
(`archive/hardware/smd2.md` — the SMD2 board whole: design, BOM, DTMF-clipping rework, only 3 ever built; `archive/hardware/smd4.md`, `smd5.md`, `bk3266.md` — the BK3266 Bluetooth module's wiring, driver, bring-up sequence and bench log —, `BOM*.md`, and the through-hole/WM8731 board's own docs under `archive/hardware/through-hole-wm8731/`). These are
**your working notes, not part of the public doc set** — useful for you when
picking up this project again, not linked from the public README. When
adding new engineering-journal-style content (debug sessions, open
questions, measured bench data), it belongs under `archive/`, not in
`docs/`. Cross-references between files already inside `archive/` keep
working unchanged (the whole subtree moved together, same relative
structure) — only links from live files (this one included) need the
`archive/` prefix.

## Key docs (read before making changes)

- [`README.md`](README.md) — public front page (also the GitHub Pages home via `_config.yml`): use cases, finer-print features, web UI thumbnails, cost table, the 4-step build guide
- Newcomer flow, each page with the same nav bar: [`docs/board.md`](docs/board.md) (1 · get a board: versions, JLCPCB ordering, the cost of a 5-board order) → [`docs/bom.md`](docs/bom.md) + [`docs/building.md`](docs/building.md) (2 · hand-solder parts, assembly incl. the SMD4 15k rework) → [`docs/flashing.md`](docs/flashing.md) (3 · toolchain, flash, console self-test) → [`docs/setup.md`](docs/setup.md) (4 · Wi-Fi, web UI tour, dial codes, LED, BOOT button, OTA)
- Public docs are also served by GitHub Pages, which only publishes `README.md` + `docs/`: link anything outside `docs/` by full `https://github.com/sdylewski/Analog-Redial/blob/main/...` URL, and never put a `.md` link inside raw HTML (Jekyll won't rewrite it)
- [`docs/features.md`](docs/features.md) — feature list, firmware architecture (components, threading model, VoIP seams)
- [`docs/hardware-smd4.md`](docs/hardware-smd4.md) — SMD4 schematic reference, GPIO pin map, power tree
- [`docs/hardware-smd5.md`](docs/hardware-smd5.md) — SMD5 board reference (Bluetooth companion, LDO, dual MCU footprint); sent to fab, not yet tested
- [`archive/signal-levels.md`](archive/signal-levels.md) — SLIC/codec levels, SHK dividers
- [`docs/voip-setup.md`](docs/voip-setup.md) — SIP server setup, dial-plan reference
- [`server/voip-provisioner/`](server/voip-provisioner/README.md) — the Asterisk-side number-provisioning service (stdlib Python, HMAC-signed protocol, per-board tokens); its board-side client is `firmware/components/voip_prov`. Tests: `python3 -m unittest test_redial_provisioner.py` in that folder
- [`firmware/README.md`](firmware/README.md) — ESP-IDF toolchain setup, build/flash
- Dial plan source of truth: `firmware/components/dialplan/` (the web UI's
  dial guide is generated from the same table). The sibling `pi_rotary_phone`
  repo is a frozen reference for the user's one Raspberry Pi unit — don't
  update it or keep it in parity.

## Firmware architecture (summary)

ESP-IDF (FreeRTOS) C, split into components so telephony logic never touches registers directly and VoIP can be added without touching the state machine: `board` → `es8388`/`ag1170` (hardware — `ag1170` drives the AG1171; the dir name predates it) → `audio`/`tones`/`dtmf` → `telephony` (per-line FSM, dial collector, dialplan) → `voip` (SIP/RTP). See `docs/features.md` §3 for the full diagram.

## SMD phone-line (tip/ring) PCB clearance rules

Applies to: `hardware/kicad/Analog Redial v4/**`, `docs/hardware-smd4.md`.

Subscriber-loop copper on this board can see **~48 V DC** off-hook feed, **~60 Vrms** ringing (**72 V** SLIC battery during ring per AG1171), and **~90 V** peaks (matches **P6KE82**-class TVS). Design spacing for **100 V peak** tip–ring.

**Clearances (2-layer, external copper, solder mask — IPC-2221 B2 baseline):**

| Between | Minimum | Project target |
|---------|---------|----------------|
| Tip ↔ ring (same line) | 0.6 mm (IPC 51–100 V external) | **0.8 mm** |
| Phone line ↔ GND / +3.3 V / +5 V / logic | 0.6 mm | **0.8 mm** |
| Line 0 phone copper ↔ Line 1 phone copper | 0.6 mm | **1.0 mm** if parallel runs are long |

Global fab minimum stays **0.15 mm**; phone-line spacing comes from the `PhoneLine` net class and `Analog Redial v4.kicad_dru`.

**KiCad setup:**
- Net class `PhoneLine` — clearance 0.8 mm, default track 0.4 mm (1.0 mm at RJ12/SLIC if space allows).
- Nets: check the current netlist (`kicad-cli sch export netlist` or the KiCad GUI) rather than trusting a hardcoded list here — auto-generated net names (`Net-(D#-K)` style) can shift between spins/re-annotations. Re-annotate to `Line0_Tip` etc. when convenient.
- DRC: `hardware/kicad/Analog Redial v4/Analog Redial v4.kicad_dru`

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

- Hardware: SMD4 is the current design — schematic unchanged from SMD3, re-routed as a 2-layer PCB. Fabricated and in use.
- Firmware: core telephony is mature and tested on earlier SMD-family hardware (hand-soldered SMD2 and SMD4, ES8388 + AG1171), selected via board Kconfig (`CONFIG_BOARD_TYPE_SMD2`/`_SMD4`/`_SMD5`); runtime `codec_detect` warns if the ES8388 doesn't answer.