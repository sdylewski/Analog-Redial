[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md)

# Features & firmware architecture

Everything Analog Redial does, and how the firmware is put together. For
using these features day to day, see [Set up & use](setup.md).

---

## 1. Hardware

| | SMD4 |
|--|------|
| MCU | ESP32-C6-DevKit |
| Codec | ES8388 @ I²C 0x10 |
| SLIC | AG1171 ×1–2 |
| Pin map | `board_pins_smd4.h` (SMD2 uses `board_pins_smd.h` instead) |

Two phone lines as codec **Left / Right** (stereo interleaved in firmware).
A single-line build can trade line 2 for **microSD** (GPIO 18–21 shared).

| Resource | ESP32-C6 |
|----------|----------|
| Hook / pulse | SLIC **SHK** → GPIO (divider on 5 V SLIC) |
| Ring | **RM** + **F/R** toggle (~20 Hz) |
| Net | Wi-Fi → SIP |

See [`hardware-smd4.md`](hardware-smd4.md) and [`signal-levels.md`](https://github.com/sdylewski/Analog-Redial/blob/main/archive/signal-levels.md).

---

## 2. Feature list

Legend: ✅ working · 🧪 experimental, off by default · 🔜 coming soon

| Feature | Status | Notes |
|------------|--------------|-------|
| **Hook detection** (off/on-hook, debounce) | ✅ | SLIC SHK debounce (~10 ms per datasheet) |
| **Pulse (rotary) dialing** | ✅ | SHK pulse counter, inter-digit timeout → digit |
| **DTMF (touch-tone) dial detection** | ✅ | Goertzel detector on ADC stream |
| **DTMF generation** | ✅ | For feedback / outbound to line |
| **Dial/ringback/busy tones, per language** | ✅ | Region-specific frequency + cadence, chosen by Settings → General → Language (`tones/tone_regions.h`, builder-editable): US default plus Italy/Germany/France/Japan/China/Spain, generic ITU-T European fallback otherwise. Sourced from ITU Operational Bulletin No. 781's country tone annex. Mechanical ringing cadence itself is not regionalized (US 2s/4s default) |
| **Per-digit dial-plan dispatch** | ✅ | Data-driven dialplan table |
| **Dial codes** (Settings → Dial Codes) | ✅ | One mutable code↔filename table covers both builtin "silly audio" clips and anything a user adds — both the dial code AND the audio file are independently editable/uploadable at runtime, no rebuild |
| **Call the other phone (inter-line, fully offline)** | ✅ | Local bridge between lines — no server, no network required |
| **Busy when other line off-hook** | ✅ | State machine |
| **Audio clip playback** | ✅ | Browser upload (Settings → Dial Codes / Operator) converts any common WAV (any sample rate/bit-depth/channel count) to 8kHz mono automatically. Optional MP3 upload support (off by default — real RAM cost). One novelty clip (a public-domain dial-up modem sound, code 412) ships by default; everything else is left for the builder to add, since most source audio is personal or copyrighted |
| **Record to web archive (211)** | ✅ | 8 kHz mono WAV, timestamped filename (UTC), downloadable/playable from `/recordings` |
| **Intercom** | 🧪 off by default (Settings → Phone, 2-line boards) | Pick up and don't dial: after a wait (default 4 s, 1–30) the other line rings with a short double ring (0.4 s, 0.2 s gap, 0.4 s, 2 s pause), distinct from an outside call. Dial a digit first and it's a normal call, so there is no mode to switch. If the other handset is already up and idle the lines connect at once; if it is busy you hear busy |
| **Voice effects during a call** | 🧪 experimental, off by default (Settings → Phone) | Press or dial **1** Dalek, **2** chipmunk, **3** Darth Vader, **4** 8-bit, **5** robot, **6** tremolo, **7** distortion, **0** normal — mid-call, on either phone; both hear it. Works on local and VoIP calls (the board where the key is pressed changes both voices). Or dial 322–324 to start a call as chipmunk, Vader or Dalek. Chipmunk and Vader add ~15 ms; the rest add no delay. On/off in Settings → Phone |
| **Pin dial codes** (Settings → Dial Codes → Pin outputs) | ✅ one-line boards without microSD | Up to four codes, each drives one of GPIO18–21 (header J5) high for 0–3600 s, or toggles it: relay, MOSFET, buzzer, light. Refused while a card is in the socket, the board is set to two lines or microSD, or a J5 shunt could connect the pin to line 1's chip (shown on the card). Same code rules as clip codes, in both directions |
| **Internet radio (8xx)** | 🔜 | Dial codes are reserved; streaming isn't implemented yet |
| **Operator help / TTS (dial 0)** | ✅ | No on-device TTS; `operator.wav` ships pre-rendered by default. Text-to-speech generation (`gen_prompt.py`, host-side) can re-render it, the record-message prompt, and the answering machine's default greeting from edited text |
| **Answering machine** (dial 899 to record a greeting) | ✅ | Per line: rings N times, plays that line's own greeting (or the shared default if none is recorded), then records the caller. Needs VOIP or a second line to actually be reachable |
| **Bluetooth (BM64 companion)** | 🔜 | Arrives with the v5 board: pair a mobile and the analog phone becomes its handset. The firmware side is written; see [`hardware-smd5.md`](hardware-smd5.md) §9 |
| **Language** | ✅ | One language for the whole phone, set at setup time — no in-use switching. UI string translations are plumbed in (`i18n_str()`) but only English ships; the language picker greys out any language without a translation file on the board |
| **Digit readback on/off** | ✅ | Explicit setting |
| **Wrong-number handling** | ✅ | Unmatched 3+ digit → prompt + dial tone |
| **VoIP outbound (555-xxxx)** | ✅ | Embedded SIP UA + RTP/G.711; digest auth |
| **VoIP inbound** | ✅ | Inbound INVITE rings the line; answer on off-hook |
| **VoIP setup (Settings → VOIP)** | ✅ | Automatic: claim a number from the [VoIP provisioner](https://github.com/sdylewski/Analog-Redial/blob/main/server/voip-provisioner/README.md) running next to Asterisk (HMAC-signed requests, per-board tokens). By hand: a web form plus a downloadable Asterisk setup script |
| **Web UI (status/log/recordings)** | ✅ | `esp_http_server`; settings page with WiFi/mode tabs |
| **WiFi provisioning (first boot / factory reset)** | ✅ | No creds, or none in range after 3 tries each → open SoftAP (named after the board once it has one) + captive setup page; BOOT-button hold (3 s) forces it without touching saved creds, (10 s) wipes them. Remembers several networks and reconnects to whichever is strongest |
| **Event log** | ✅ | Two tiers: a short activity feed (calls, recordings, dial errors) on the Status page, and the full ring-buffer debug log under Settings → Diagnostics |

---

## 3. Firmware architecture

ESP-IDF (FreeRTOS) C, organized as components with a hardware-abstraction
split so the **telephony logic never talks to registers directly** and the
**VoIP path can be added without touching the state machine**.

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
   │ per-line FSM    │◄──────►│ I²S engine 48k    │◄──────►│ SIP/RTP          │
   │ dial collector  │        │ tone/dtmf gen     │  RTP   │                  │
   │ dialplan table  │        │ dtmf detect       │        └──────────────────┘
   └───────┬─────────┘        │ mixer / bridge    │
           │                  └─────────┬─────────┘
           ▼                            ▼
   ┌─────────────────┐        ┌──────────────────┐
   │      slic       │        │     es8388       │
   │ SHK/PD/FR/RM    │        │ I²C config       │
   │ hook+pulse FSM  │        │ route/vol/power  │
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
| `board` | Single source of truth for GPIO numbers + board-level init; probes the I2C bus and warns if the flashed config doesn't match the codec found |
| `es8388` | Codec: I²C register driver, init sequence, output routing, ADC input-line select, volume |
| `audio` | I²S std-mode master engine; TX/RX DMA; per-channel tone injection; software mixer + inter-line bridge; calls `tones`/`dtmf` |
| `tones` | Sine/dual-tone generator; call-progress tones (dial/ringback/busy) with cadence |
| `dtmf` | DTMF row/col synthesis + Goertzel decoder (8 freqs) |
| `ag1170` (SLIC) | Per-line SLIC control: SHK debounce, pulse-dial detection FSM, ring cadence task, PD/FR/RM |
| `telephony` | Per-line call state machine; merges pulse + DTMF digits into a dial buffer; drives tones, ring, audio routing; invokes dialplan; exposes hooks for VoIP |
| `dialplan` | Data-driven table of codes/services + mode flags |
| `voip` | Clean API (`register/dial/answer/hangup` + event callbacks); embedded SIP/RTP |
| `bringup` | `esp_console` commands to validate hardware step-by-step |

The diagram above is the signal path specifically — the telephony/audio/VoIP
chain this section is about. Everything around it that makes the device
usable is its own set of components too, not pictured since they don't sit
on that signal path: `webui` (HTTP server, all settings pages) / `webauth`
(the optional web password and sign-in sessions), `wifi`
(station/AP mode, the multi-network connection manager, mDNS), `storage` /
`sdcard` (internal flash + microSD, both behind one API), `prompts` / `i18n`
(text-to-speech prompt tracking, UI string translation), `bt_companion`
(BM64 UART bridge), `idle_power` (Wi-Fi/CPU power saving), and
`activity`/`logbuf`/`logctl` (the Status page's activity feed, the full
debug log, and runtime log-level control).

### Threading model (FreeRTOS tasks)

| Task | Job |
|------|-----|
| `audio_io` | High-priority I²S read/write loop, fixed frame size (e.g. 10 ms) |
| `line0_fsm`, `line1_fsm` | Per-line telephony state machines |
| `ring0/1` | Ring cadence (spawned on demand) |
| `voip` | SIP signaling + RTP |
| `console`/`httpd` | Bring-up console / web UI |

### VoIP-ready seams

1. **`audio` exposes channel taps**: `audio_get_rx_frame(line)` /
   `audio_put_tx_frame(line)` in 8 kHz mono PCM (resampled from the 48 kHz
   codec stream). RTP just reads/writes these.
2. **`telephony` calls an abstract call backend**: local inter-line bridge
   *or* `voip`. The FSM doesn't care which; it calls
   `call_backend->dial()/answer()/hangup()`.
3. **`voip` events** (`INCOMING`, `ANSWERED`, `CLOSED`) feed the same FSM
   inputs the local bridge uses.

This means the entire telephony stack (dialing, tones, ring, dial-plan) is
testable and useful **before** any SIP code exists — and it's why the board
can act as a standalone two-phone exchange with zero network dependency.

---

## 4. Bring-up console

The serial console's hardware-test commands are listed in
[Flash & test](flashing.md#6-test-the-hardware); type `help` on the console
for all of them. Toolchain and build-system detail:
[`firmware/README.md`](https://github.com/sdylewski/Analog-Redial/blob/main/firmware/README.md).
