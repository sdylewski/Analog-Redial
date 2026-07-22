# ESP32 port plan — Pi v1.0 → unified firmware

Maps the working **Pi** stack ([`pi_rotary_phone`](../pi_rotary_phone), `telephone.py` **v1.0**) onto **ESP32-C6** firmware in this repo. Use this before large refactors.

**Authoritative dial plan:** [`pi_rotary_phone/docs/DIAL_CODES.md`](../pi_rotary_phone/docs/DIAL_CODES.md) — `dialplan`'s TABLE and `FEATURES.md` now match it.

---

## Hardware sub-projects (one firmware tree)

| Kconfig target | PCB | Codec | SLIC | Pin map header |
|----------------|-----|-------|------|----------------|
| **SMD + ES8388** | `Rotary_phone_SMD2` | ES8388 @ I²C **0x10** | AG1171 ×1–2 | `board_pins_smd.h` |
| **Through-hole + WM8731** | `rotary_phone_gateway` | WM8731 @ I²C **0x1A** | AG1170 ×2 | `board_pins.h` |

**Goal:** `menuconfig` → **Board type** selects pin map + codec backend; telephony/dialplan/voip stay shared.

### Planned Kconfig (not wired yet)

```
Rotary Phone Gateway Configuration
  ├── Board type
  │     ├── SMD (ES8388, AG1171)     → CONFIG_BOARD_SMD=y
  │     └── Through-hole (WM8731, AG1170) → CONFIG_BOARD_THT=y
  ├── Number of phone lines (1 or 2)
  └── microSD (SMD build B only)
```

Implementation sketch:

- `board/board_config.h` — includes the right pins from `board_pins_smd.h` or `board_pins.h`
- `codec/` abstraction — `codec_init()`, `codec_set_volume()`, route L/R to lines
  - Backends: `wm8731` (existing), `es8388` (new; use Espressif or vendor register tables)
- `slic/` rename from `ag1170` — same GPIO API; AG1170/AG1171 compatible for this design

---

## Pi vs ESP32 — architecture difference

| | Pi (dev reference) | ESP32 (through-hole / SMD) |
|--|--------------|------------------------|
| Talk audio | 2× USB sound dongles | **Codec** stereo L/R → 2× SLIC analog path |
| SLIC | Hook + ring only | Hook + ring + **line audio** |
| VoIP | **baresip** (systemd) + ctrl_tcp | **Embedded SIP/RTP** on Wi-Fi |
| Inter-phone | **sox** ALSA bridge | Software L↔R in `audio` |
| Web UI | Flask + SQLite | `esp_http_server` + NVS/SPIFFS |
| TTS | Piper / espeak on Pi | **Pre-rendered WAV** in flash |
| Radio | mpv / ffmpeg | HTTP stream decoder (library TBD) |

---

## Feature port matrix (Pi v1.0 baseline)

Legend: ✅ done or close · 🟡 partial · 🔜 not started · ❌ N/A on MCU

| Feature | Pi v1.0 | ESP32 today | Port phase | Notes |
|---------|---------|-------------|------------|-------|
| Hook + SHK debounce | ✅ | ✅ `ag1170` | — | Match Pi pulse tuning next |
| Pulse dial (dual-edge) | ✅ | 🟡 | **1** | Port constants from [`PULSE_DIALING.md`](../pi_rotary_phone/docs/PULSE_DIALING.md) |
| DTMF detect | ✅ | ✅ | **1** | Align with pulse suppression |
| Dial / ringback / busy | ✅ | ✅ | — | |
| Mechanical ring | ✅ | ✅ | — | |
| Dial plan (0, 2xx, 3xx, 4xx, 8xx, 555, 9xx) | ✅ | 🟡 | **1** | Codes now match `DIAL_CODES.md`; 2xx/8xx actions still TODO (rows below) |
| Inter-phone 3xx + voice variants | ✅ | ✅ | — | 321–324, symmetric on both lines (matches live Pi config); bridge in `audio` |
| Play vs call-only 931/932 | ✅ | ✅ | — | Mode flags |
| Digit readback 921/922 | ✅ | ✅ | — | |
| Language 901/902 | ✅ | 🟡 | **2** | Pre-baked EN/IT WAV sets |
| Operator 0 (TTS help) | ✅ | 🟡 | **2** | Flash WAVs only |
| Clips 4xx | ✅ | 🟡 | **2** | SPIFFS / partition |
| Record 211 | ✅ | ✅ | — | `recorder` component: 8 kHz mono WAV to `storage`, auto-stop on hangup/silence/30 s |
| Callback 212–215 | ✅ | ✅ | — | Record then play back on the handset; high/low via resample, Dalek flat (ring-mod TODO) |
| Internet radio 811/812 | ✅ | 🔜 | **3** | [`esp-adf`](https://github.com/espressif/esp-adf) or `esp_http_client` + decoder |
| VoIP 555-xxxx + inbound | ✅ | ✅ | — | Custom SIP UA (`voip.c`) + RTP/G.711 (`rtp.c`); digest auth; registers + calls a real Asterisk (bench-verified) |
| Web status / log / recordings | ✅ | ✅ | — | `esp_http_server`; status/log/recordings pages done |
| Config JSON / settings UI | ✅ | 🟡 | **4** | Modes + WiFi + board name persist to NVS, settings page has real tabs for them; interphone/VOIP tabs still read-only placeholders (no on-device TTS gen) |
| WiFi provisioning / factory reset | n/a | ✅ | — | First boot / GPIO9-erased creds → SoftAP `RotaryPhone-XXXX` + captive setup page |

### Phases

1. **Telephony parity** — pulse FSM, dial plan, inter-phone, board Kconfig, ES8388 bring-up on SMD
2. **Assets** — SPIFFS clips, operator WAVs, record flows
3. **Network media** — HTTP radio
4. **VoIP + ops** — SIP stack, provisioning, lightweight web UI

---

## External libraries (prefer over custom)

| Need | Candidates | Notes |
|------|------------|-------|
| SIP + RTP | [esp-idf-sip](https://github.com/espressif/esp-idf/tree/master/examples/protocols/sip), **PJSIP** (heavier), **reSIProcate** / **baresip** port | Pi uses baresip; evaluate RAM/flash on C6 4–16 MB |
| HTTP audio | **esp-adf** `audio_pipeline`, or **libhelix-mp3** + custom HTTP | For 811/812 |
| JSON config | **cJSON** (IDF bundled) | Port `phone_config.json` subset |
| Web server | **esp_http_server** (IDF) | Status + download recordings |
| File storage | **SPIFFS** (internal) done; **FAT on microSD** (build B) done via `sdcard` component, opt-in `CONFIG_BOARD_MICROSD` + 1 line | Recordings + clips |
| ES8388 | Register driver from vendor / Espressif codec component | Wrap behind `codec/` API |
| Wi-Fi provisioning | **wifi_provisioning** (IDF) | Replace Pi static IP docs for field install |

Avoid reimplementing: Goertzel (keep `dtmf`), I²S (keep `audio`), GPIO SLIC (keep `ag1170`).

---

## Code refactor direction

Current gaps (July 2026):

- Firmware builds **WM8731** + `board_pins.h` only; SMD hardware is **ES8388** + `board_pins_smd.h`
- ~~`voip` is a stub~~ — done: custom SIP/RTP UA registers + calls a real Asterisk
- No `Kconfig` board selection

Suggested module layout after refactor:

```text
components/
  board/          pin maps + board_detect_stuffing() (optional)
  codec/          abstract API
    wm8731/       THT backend
    es8388/       SMD backend
  slic/           (rename from ag1170)
  audio/          unchanged seam
  telephony/      port Pi pulse FSM
  dialplan/       sync with DIAL_CODES.md
  voip/           SIP adapter (library inside)
  storage/        SPIFFS + optional SD
  httpd/          web UI (later)
```

**OK to rewrite** bring-up and dialplan; keep `audio` / `telephony` boundaries.

---

## Verification checklist (before calling “Pi parity”)

- [ ] Pulse timing matches Pi [`PULSE_DIALING.md`](../pi_rotary_phone/docs/PULSE_DIALING.md) on real handset
- [ ] 3xx inter-phone: busy when other line off-hook; voice variants audible
- [ ] 555-xxxx reaches test Asterisk extension (when VoIP phase done)
- [ ] SMD board: ES8388 loopback + SLIC line 1/2 audio
- [ ] THT board: WM8731 loopback unchanged
- [ ] Build A/B stuffing matches `CONFIG_BOARD_*`

---

## Future ideas / parking lot (not scheduled)

- **One language per phone, set at config/setup time — drop in-use switching.**
  The Pi lets you switch language mid-use (dial 901/902, per-line `phoneLang`).
  That's more complication than it's worth on the ESP32: instead, pick a single
  language when setting the phone up, generate that language's prompts/number
  audio once during setup, and don't offer runtime switching. Removes the
  901/902 codes, the per-line language flag in `dialplan_modes_t`, and the
  call-only-forces-Italian coupling; simplifies the operator/record prompt
  pipeline to a single asset set.

- **User audio manager + dial-plan editor (both Pi and ESP32).** Today the
  clip codes (401/411/…) are hard-coded to filenames in `dialplan.c`. Idea: a
  Settings screen to **add an audio entry** — upload a WAV/song, pick a dial
  code (reject one already in use), name it, optionally set a length — and
  have it register into the dial plan at runtime (config-driven table instead
  of the compile-time `TABLE[]`). Likely a modal/popup on the Pi; on the
  ESP32 it'd be a plain sub-page (the ESP32 web UI avoids client JS). The
  ESP32 is the primary UI target since the board is being built for it.
  Recordings vs. built-in assets are already separated (Recordings tab shows
  only `rec_*`; assets are managed under Settings → Storage), which is the
  groundwork for this.

- **Pi-based header-board hardware variant (doesn't exist yet).** A future
  revision of the current SMD board where the **ESP32-C6 is swapped for a
  Raspberry Pi header** (Pi Zero / CM form factor), reusing the same on-board
  digital audio codec + SLIC front-end and only changing the microcontroller.
  Would make the `pi_rotary_phone` stack run on this project's hardware rather
  than the old USB-dongle rig. Board-identity reporting already distinguishes
  hardware types, so such a board would just report as a Pi variant.

---

## Related docs

| Doc | Role |
|-----|------|
| [`FEATURES.md`](FEATURES.md) | Architecture diagram + component list (update as code lands) |
| [`hardware/BOM.md`](hardware/BOM.md) | SMD vs through-hole BOM index |
| [`firmware/README.md`](firmware/README.md) | Build / flash |
| [`pi_rotary_phone/README.md`](../pi_rotary_phone/README.md) | Reference behavior |
