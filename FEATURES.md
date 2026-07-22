# ESP32-C6 Rotary Phone Gateway — Feature Port & Architecture

Maps the **Pi v1.0** stack ([`pi_rotary_phone`](../pi_rotary_phone)) onto ESP32 firmware. For the full port scope, phases, and dual-board plan, see **[`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md)**.

The ESP32 build is **not** a line-by-line port: Pi used USB dongles + baresip + Flask; ESP32 uses **on-chip I²S codec**, **GPIO SLIC**, and (eventually) **embedded SIP** over Wi-Fi.

**Dial plan source of truth:** [`pi_rotary_phone/docs/DIAL_CODES.md`](../pi_rotary_phone/docs/DIAL_CODES.md) — `dialplan`'s TABLE now matches these codes exactly.

---

## 1. Hardware — two board types, one firmware tree

| | **Through-hole** (`rotary_phone_gateway`) | **SMD** (`Rotary_phone_SMD2`) |
|--|-------------------------------------------|-------------------------------|
| MCU | ESP32-C6-DevKitM-1 | ESP32-C6-WROOM-1 |
| Codec | **WM8731** @ I²C 0x1A | **ES8388** @ I²C 0x10 |
| SLIC | **AG1170** ×2 | **AG1171** ×1–2 |
| Pin map | `board_pins.h` | `board_pins_smd.h` |
| BOM | [`hardware/BOM-through-hole.md`](hardware/BOM-through-hole.md) | [`hardware/BOM-smd.md`](hardware/BOM-smd.md) |

Both carry **two phone lines** as codec **Left / Right** (stereo interleaved in firmware). SMD build B can trade line 2 for **microSD** (GPIO 18–21 XOR).

| Resource | ESP32-C6 |
|----------|----------|
| Hook / pulse | SLIC **SHK** → GPIO (divider on 5 V SLIC) |
| Ring | **RM** + **F/R** toggle (~20 Hz) |
| Net | Wi-Fi → SIP (planned) |

See `hardware/WM8731.md` / `hardware/ROTARY_PHONE_SMD.md` and `HARDWARE_SIGNAL_LEVELS.md`.

---

## 2. Feature port matrix

Legend: ✅ planned/implemented in firmware · 🟡 partial/simplified · 🔜 stubbed
interface, implemented later · ❌ dropped (host/Pi-only concept)

| Pi feature | ESP32 status | Notes |
|------------|--------------|-------|
| **Hook detection** (off/on-hook, debounce) | ✅ | `ag1170` SHK debounce (~10 ms per datasheet) |
| **Rotary pulse dialing** | ✅ | SHK pulse counter, inter-digit timeout → digit |
| **DTMF (tone) dial detection** | ✅ | Goertzel detector on ADC stream |
| **DTMF generation** | ✅ | Added (Pi had none) — for feedback / outbound to line |
| **Dial tone** (350+450) | ✅ | `tones` synth, injected to line via DAC |
| **Ringback** (440+480, 2s/4s) | ✅ | `tones` cadence |
| **Busy tone** (480+620, 0.5s) | ✅ | `tones` cadence |
| **Mechanical ringing** | ✅ | `ag1170` RM + F/R cadence (US 2s/4s default) |
| **Per-digit dial-plan dispatch** | ✅ | `dialplan` table ported from `DIAL_CODES.md` |
| **Dial codes** (Settings → Dial Codes) | ✅ | One mutable code↔filename table covers both the builtin "silly audio" clips and anything a user adds — both the dial code AND the audio file are independently editable/uploadable at runtime, no rebuild (not part of `DIAL_CODES.md`, an ESP32-only addition; see `dialplan_clip_entry_t`) |
| **Call the other phone (inter-line)** | ✅ | Local bridge L↔R channels in `audio` |
| **Busy when other line off-hook** | ✅ | state machine |
| **Audio clip playback** (silly clips 4xx, banana line) | ✅ | Browser upload (Settings → Dial Codes / Operator) converts any common WAV (any sample rate/bit-depth/channel count) to 8kHz mono via `audio_convert`, or bring your own via `sync_pi_audio.py`/`gen_prompt.py`. Optional MP3 upload support (`audio_convert`'s Kconfig, off by default — real RAM cost, see that Kconfig's help). No clip WAVs ship by default (copyright). |
| **Record + playback (212/213/214/215)** | ✅ | `recorder` captures 8 kHz mono → plays back; high/low pitch via resample, Dalek flat (ring-mod TODO) |
| **Record to web archive (211)** | ✅ | `recorder` → 8 kHz mono WAV in `storage`, downloadable from `/recordings` |
| **Voice effects** (pitch/Dalek) | 🟡 | Simple on-device pitch shift; Dalek = ring-mod; not `sox`-grade |
| **Internet radio (8xx)** | 🟡 | HTTP/Shoutcast stream → I²S; later milestone |
| **Operator help / TTS (dial 0)** | ✅ | No on-device TTS; `operator.wav` ships by default (pre-rendered, baked into the `bundled` partition at build time) |
| **Language** | ✅ | Deliberately not ported as-is: one language for the whole phone, set at setup time (`board_language_set`/Settings), no in-use switching or 901/902 codes |
| **Digit readback on/off (921/922)** | ✅ | Mode flag, explicit set (not a toggle, matches Pi) |
| **Wrong-number handling** | ✅ | Unmatched 3+ digit → prompt + dialtone |
| **VoIP outbound (555-xxxx)** | ✅ | Custom embedded SIP UA + RTP/G.711; digest auth; tested registering + calling a real Asterisk |
| **VoIP inbound (ext 1001/1002)** | ✅ | Inbound INVITE rings the line; answer on off-hook (signaling path implemented, bench-tested outbound) |
| **VoIP setup (Settings → VOIP)** | ✅ | Web form (server/extension/user/password per line) calling `voip_set_config()`, plus a downloadable Asterisk server-script generator (marker-tagged pjsip.conf/extensions.conf blocks, safe alongside other phones on the same box) |
| **Web UI (status/log/recordings)** | ✅ | `esp_http_server`; settings page has real tabs for modes/WiFi, placeholders for the rest |
| **WiFi provisioning (first boot / factory reset)** | ✅ | No creds → open SoftAP `RotaryPhone-XXXX` + captive setup page; GPIO9 (BOOT) 3 s hold wipes creds |
| **Event log (SQLite)** | 🟡 | Replaced by ring-buffer log + optional HTTP/JSON |
| **Single-instance lock** | ❌ | N/A on MCU |

---

## 3. Firmware architecture

ESP-IDF (FreeRTOS) C, organized as components with a hardware-abstraction split
so the **telephony logic never talks to registers directly** and the **VoIP path
can be added without touching the state machine**.

```text
                         ┌─────────────────────────────┐
                         │        app_main             │
                         │  normal-run | bringup console│
                         └──────────────┬──────────────┘
                                        │
            ┌───────────────────────────┼───────────────────────────┐
            ▼                           ▼                           ▼
   ┌─────────────────┐        ┌──────────────────┐        ┌──────────────────┐
   │   telephony     │        │      audio        │        │      voip        │
   │ per-line FSM    │◄──────►│ I²S engine 48k    │◄──────►│ SIP/RTP (stub →  │
   │ dial collector  │        │ tone/dtmf gen     │  RTP   │ embedded later)  │
   │ dialplan table  │        │ dtmf detect       │        └──────────────────┘
   └───────┬─────────┘        │ mixer / bridge    │
           │                  └─────────┬─────────┘
           ▼                            ▼
   ┌─────────────────┐        ┌──────────────────┐
   │     ag1170      │        │ wm8731 / es8388   │
   │ SHK/PD/FR/RM    │        │ I²C config        │
   │ hook+pulse FSM  │        │ route/vol/power   │
   │ ring cadence    │        └─────────┬─────────┘
   └───────┬─────────┘                  │
           ▼                            ▼
   ┌──────────────────────────────────────────────┐
   │                  board (GPIO map)             │
   └──────────────────────────────────────────────┘
```

### Components

| Component | Responsibility |
|-----------|----------------|
| `board` | Single source of truth for GPIO numbers + board-level init; `board_detect_codec()` probes the I2C bus and warns if the flashed board type doesn't match the codec found |
| `wm8731` | THT board codec: I²C register driver, init sequence, input/output routing, volume, power-down, sample-rate table |
| `es8388` | SMD board codec: I²C register driver, init sequence, output routing, ADC input-line select (see limitation below), volume |
| `audio` | I²S std-mode master engine; TX/RX DMA; per-channel tone injection; software mixer + inter-line bridge; calls `tones`/`dtmf` |
| `tones` | Sine/dual-tone generator; call-progress tones (dial/ringback/busy) with cadence |
| `dtmf` | DTMF row/col synthesis + Goertzel decoder (8 freqs) |
| `ag1170` | Per-line SLIC control: SHK debounce, pulse-dial detection FSM, ring cadence task, PD/FR/RM |
| `telephony` | Per-line call state machine; merges pulse + DTMF digits into a dial buffer; drives tones, ring, audio routing; invokes dialplan; exposes hooks for VoIP |
| `dialplan` | Data-driven table of codes/services (ported from `DIAL_CODES.md`) + mode flags |
| `voip` | Clean API (`register/dial/answer/hangup` + event callbacks); stub now, embedded SIP/RTP later |
| `bringup` | `esp_console` commands to validate hardware step-by-step |

### Threading model (FreeRTOS tasks)

| Task | Job |
|------|-----|
| `audio_io` | High-priority I²S read/write loop, fixed frame size (e.g. 10 ms) |
| `line0_fsm`, `line1_fsm` | Per-line telephony state machines (or one task iterating both) |
| `ring0/1` | Ring cadence (spawned on demand) |
| `voip` | SIP signaling + RTP (later) |
| `console`/`httpd` | Bring-up console / web UI |

### VoIP-ready seams (so VoIP can drop in last)

1. **`audio` exposes channel taps**: `audio_get_rx_frame(line)` / `audio_put_tx_frame(line)` in 8 kHz mono PCM (resampled from the 48 kHz codec stream). RTP just reads/writes these.
2. **`telephony` calls an abstract call backend**: local inter-line bridge *or* `voip`. The FSM doesn't care which; it calls `call_backend->dial()/answer()/hangup()`.
3. **`voip` events** (`INCOMING`, `ANSWERED`, `CLOSED`) feed the same FSM inputs the local bridge uses.

This means the entire telephony stack (dialing, tones, ring, dial-plan) is
testable and useful **before** any SIP code exists.

---

## 4. Milestones

1. **Board bring-up** (this pass): GPIO map, I²C scan, codec probe/config (WM8731 or ES8388), I²S tone out, ADC capture, SLIC hook/ring, pulse + DTMF dialing — all exercised by the `bringup` console and the host-side scripts in `firmware/bringup/`.
2. **Local telephony**: dial tone → collect digits → dial-plan → inter-line calls, ring, busy, call progress.
3. **Assets**: flash filesystem + prompt/clip playback, record/playback, effects.
4. **Web UI**: status/log/recordings over `esp_http_server`.
5. **VoIP**: embedded SIP register + G.711 RTP over Wi-Fi to external PBX; wire into the `voip` seam.

---

## 5. Bring-up test commands (serial console)

Run on the DevKit (works with no PCB attached; hardware-dependent commands
degrade gracefully):

| Command | Tests |
|---------|-------|
| `i2c_scan` / `codec_detect` | I²C bus + codec presence (ES8388 0x10 / WM8731 0x1A), board-type mismatch warning |
| `codec_init` / `codec_dump` | Codec config + register readback path |
| `tone <hz> [ms] [line]` | DAC → line audio generation |
| `dialtone`/`ringback`/`busy <line>` | Call-progress tone synthesis |
| `loopback` | ADC → DAC passthrough (codec + I²S sanity) |
| `capture <ms>` | ADC RMS / level meter |
| `slic <line> <pd|fr|rm> <0|1>` | Raw SLIC line control |
| `ring <line> [secs]` | Mechanical ring cadence |
| `hookmon` | Live SHK state for both lines |
| `dialmon` | Decode pulse + DTMF digits live |
| `dtmf_gen <digits>` | Emit DTMF on a line |

See `firmware/README.md` for build/flash/run.
