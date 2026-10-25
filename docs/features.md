[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md)

# Features & firmware architecture

Everything Analog Redial does, and how the firmware is put together. For
using these features day to day, see [Set up & use](setup.md).

---

## 1. Hardware

| | SMD4 (v4) | SMD5 (v5) |
|--|------|------|
| MCU | ESP32-C6-DevKitC-1 | ESP32-C6-DevKitC-1, or a 30-pin clone (see the warning in [`hardware-smd5.md`](hardware-smd5.md#10-mcu-module-variants)) |
| Codec | ES8388 @ I²C 0x10 | same |
| SLIC | AG1171 ×1–2 | same |
| Expansion header | none | 8-pin **J2**: 3.3 V, a serial port, a control pin and line 0's audio; currently used for the Bluetooth board |
| Pin map | `board_pins_smd4.h` (SMD2 uses `board_pins_smd.h` instead) | reuses SMD4's |

Two phone lines as codec **Left / Right** (stereo interleaved in firmware).
A single-line build can trade line 2 for **microSD** (GPIO 18–21 shared).

| Resource | ESP32-C6 |
|----------|----------|
| Hook / pulse | SLIC **SHK** → GPIO (divider on 5 V SLIC) |
| Ring | **RM** + **F/R** toggle (~20 Hz) |
| Net | Wi-Fi → SIP |

See [`hardware-smd4.md`](hardware-smd4.md), [`hardware-smd5.md`](hardware-smd5.md) and [`signal-levels.md`](https://github.com/sdylewski/Analog-Redial/blob/main/archive/signal-levels.md).

---

## 2. Feature list

Legend: ✅ working · 🧪 experimental, off by default · 🔜 coming soon · 💡 idea, not built

| Feature | Status | Notes |
|------------|--------------|-------|
| **Hook detection** (off/on-hook, debounce) | ✅ | SLIC SHK debounce (~10 ms per datasheet) |
| **Pulse (rotary) dialing** | ✅ | SHK pulse counter, inter-digit timeout → digit |
| **DTMF (touch-tone) dial detection** | ✅ | Goertzel detector on ADC stream |
| **DTMF generation** | ✅ | For feedback / outbound to line |
| **Dial/ringback/busy tones, per language** | ✅ | Region-specific frequency + cadence, chosen by the dial tone region on Settings → Phone, which can follow the UI language (`tones/tone_regions.h`, builder-editable): US default plus Italy/Germany/France/Japan/China/Spain, generic ITU-T European fallback otherwise. Sourced from ITU Operational Bulletin No. 781's country tone annex. Mechanical ringing cadence itself is not regionalized (US 2s/4s default) |
| **Per-digit dial-plan dispatch** | ✅ | Data-driven dialplan table |
| **Dial codes** (Settings → Dial Codes) | ✅ | One mutable code↔filename table covers both builtin "silly audio" clips and anything a user adds — both the dial code AND the audio file are independently editable/uploadable at runtime, no rebuild |
| **Call the other phone (inter-line, fully offline)** | ✅ | Local bridge between lines — no server, no network required |
| **Busy when other line off-hook** | ✅ | State machine |
| **Audio clip playback** | ✅ | Browser upload (Settings → Dial Codes / Operator) converts any common WAV (any sample rate/bit-depth/channel count) to 8kHz mono automatically. Optional MP3 upload support (off by default — real RAM cost). Two clips ship by default (a public-domain dial-up modem sound, code 412, and the Jenny message, code 8675309); everything else is left for the builder to add, since most source audio is personal or copyrighted |
| **Record to web archive (211)** | ✅ | 8 kHz mono WAV, timestamped filename (UTC), downloadable/playable from `/recordings` |
| **Intercom** | 🧪 off by default (Settings → Phone, 2-line boards) | Pick up and don't dial: after a wait (default 4 s, 1–30) the other line rings with a short double ring (0.4 s, 0.2 s gap, 0.4 s, 2 s pause), distinct from an outside call. Dial a digit first and it's a normal call, so there is no mode to switch. If the other handset is already up and idle the lines connect at once; if it is busy you hear busy |
| **Voice effects during a call** | 🧪 experimental, off by default (Settings → Phone) | Press or dial **1** Dalek, **2** chipmunk, **3** Darth Vader, **4** 8-bit, **5** robot, **6** tremolo, **7** distortion, **0** normal — mid-call, on either phone; both hear it. Works on local and VoIP calls (the board where the key is pressed changes both voices). Or dial 322–324 to start a call as chipmunk, Vader or Dalek. Chipmunk and Vader add ~15 ms; the rest add no delay. On/off in Settings → Phone |
| **Pin dial codes** (Settings → Dial Codes → Pin outputs) | ✅ one-line boards without microSD | Up to four codes, each drives one of GPIO18–21 (header J5) high for 0–3600 s, or toggles it: relay, MOSFET, buzzer, light. Refused while a card is in the socket, the board is set to two lines or microSD, or a J5 shunt could connect the pin to line 1's chip (shown on the card). Same code rules as clip codes, in both directions |
| **Internet radio** | 💡 | An idea, not built: dial a code and stream a station. No dial codes are assigned |
| **Operator help / TTS (dial 0)** | ✅ | No on-device TTS; `operator.wav` ships pre-rendered by default. Text-to-speech generation (`gen_prompt.py`, host-side) can re-render it, the record-message prompt, the timer's question and message, and the answering machine's default greeting from edited text; with a VoIP provisioner that offers text-to-speech, the board does it itself ("Save & generate on server", "Update spoken audio") |
| **Answering machine** (dial 899 to record a greeting) | ✅ | Per line: rings N times, plays that line's own greeting (or the shared default if none is recorded), then records the caller. Needs VOIP or a second line to actually be reachable |
| **Bluetooth (BM64 companion)** | ✅ v5 board | Pair a mobile and the analog phone becomes its handset: the bell rings for an incoming call, lift to answer, hang up to end it, caller ID in the activity log, and dial out with 1 + area code + number. Music playing from the mobile comes through the handset when you lift it, with no dial tone over it. The BM64's audio configuration is written to the module automatically the first time (needs its two programming leads once; see [`hardware-smd5.md`](hardware-smd5.md) §9). The cheaper DFRobot BT401 module has a driver too (Settings → General → Board hardware), but hasn't been tested on a real module yet |
| **Home Assistant** (Settings → HA) | 🧪 | Webhook and MQTT auto-discovery: every pickup, hang-up, dial code and ring becomes an event; Home Assistant can ring a phone or fire a pin output back. Verified against Home Assistant Core (events, discovery, the ring button); the REST sensor and command examples are untested. See [Home Assistant](home-assistant.md) |
| **Low-power idle** (Settings → Hardware) | ✅ on by default | After a few idle minutes (default 5) the phone-line chips are polled instead of left on, the audio pipeline pauses and the CPU clock scales down; a pickup restores everything. Wi-Fi power-save is added too, with or without a VoIP server: the radio then wakes for each router beacon, so an incoming call can take about 0.1-0.3 s longer to start ringing (longer on a router set to a long beacon interval). Each option can be switched off |
| **Line audio level check** (Settings → Diagnostics) | ✅ | Plays a test tone on a line, steps the earpiece level down from the default until distortion is under 5%, and recommends a level; one button applies it |
| **Timer (dial 846)** | ✅ | Asks "How many minutes for the timer?" (the clip is `timer_prompt.wav`; a short beep stands in if it's missing), then takes one or two digits: 1-99 minutes, 0 cancels. A single digit starts after 3 seconds. When time is up an on-hook phone rings 6 seconds, four times 20 seconds apart, even in quiet hours, until picked up; a phone that's already off-hook on dial tone hears the message at once. Lifting the handset plays the spoken message (default "Your timer is done."; the clip is `timer_done.wav`, and a beep stands in if it's missing). Both the question and the message have editable text on the Timer card under Settings → Dial Codes. A single digit followed by hanging up also starts it. One timer per line, kept in memory only |
| **Firmware updates from the web page** | ✅ | Settings → Hardware → Firmware: **Check for updates** reads the release list from GitHub (through your browser) and **Install** hands the right image to the board, which installs it into the spare slot and reverts by itself if it fails to start. A board with the old flash layout needs one USB reflash first. Uploading a .bin by hand still works |
| **Call recording** (Settings → Diagnostics) | ✅ | Arm it, lift a handset: the whole call (dial tone, dialing, voice) is saved to the microSD card as a stereo WAV, then it switches itself off |
| **Power-supply warning** | ✅ | Restarts are counted in flash (Settings → Hardware → Restarts). After a brownout restart: an amber blink on the status LED, a banner on every page, and a falling tone on the next pickup |
| **Language** | ✅ | One language for the whole phone, set at setup time — no in-use switching. UI string translations are plumbed in (`i18n_str()`) but only English ships; the language picker greys out any language without a translation file on the board |
| **Digit readback on/off** | ✅ | Explicit setting (dial 821/822). Each dialled digit is spoken from the bundled `0.wav`..`9.wav`, or a touch-tone burst if those are missing |
| **Pi (dial 314)** | ✅ | Reads "three point one four one five..." aloud, 3 and the first 100 decimals, one digit clip at a time after a one-second pause, with a short breath after every 5 digits (about 50 seconds in all). A built-in code (recodable under Dial Codes); hang up to stop |
| **Wrong-number handling** | ✅ | Unmatched 3+ digits → reorder tone for 3 s, then dial tone |
| **VoIP outbound (555-xxxx)** | ✅ | Embedded SIP UA + RTP/G.711; digest auth |
| **VoIP inbound** | ✅ | Inbound INVITE rings the line; answer on off-hook |
| **VoIP setup (Settings → VOIP)** | ✅ | Automatic: claim a number from the [VoIP provisioner](https://github.com/sdylewski/Analog-Redial/blob/main/server/voip-provisioner/README.md) running next to Asterisk (HMAC-signed requests, per-board tokens). By hand: a web form plus a downloadable Asterisk setup script |
| **Board-to-board calls, local VoIP (Settings → VOIP)** | ✅ | Boards on the same Wi-Fi call each other with no server, for phones that can't share one two-line board: each line gets a free 4-digit number the first time the board is on your network (starting with 6, 7 or 9, never 1, which Bluetooth dial-out uses; change it any time), and other boards dial it. The same four digits are the default extension on a VoIP server (other phones there dial 555 + the digits). Phones on the network that have no number yet are listed as "not set up". Boards find each other over mDNS and call directly (SIP + RTP, same audio path as VoIP); a number already used by another board is refused. The VoIP server section starts folded away until VoIP is turned on |
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
(BM64 UART link, pairing, call control and the one-time audio setup), `idle_power` (Wi-Fi/CPU power saving), and
`activity`/`logbuf`/`logctl` (the Status page's activity feed, the full
debug log, and runtime log-level control), `webhook` / `mqtt_ha` (Home Assistant), `callcap` (call recording), `player` / `recorder` (clips and messages), `aec` (echo cancellation) and `voip_prov` (number provisioning).

### Threading model (FreeRTOS tasks)

| Task | Job |
|------|-----|
| `audio_io` | High-priority I²S read/write loop, 10 ms frames at 48 kHz; tones, bridging and taps |
| `tel_fsm` | The telephony state machine for both lines: hook, digits, tones, calls, timers |
| `ag1170` | SLIC monitor: hook debounce, pulse counting, ring cadence |
| `sip`, `rtp_rx`, `rtp_tx` | VoIP signalling and G.711 media |
| `player`, `recorder` | Clip playback and message recording, one task per clip or recording |
| `homecall` | Finds other boards on the network and advertises this one |
| `wifi_mgr` | Wi-Fi connection manager (saved networks, setup network) |
| `console` / `httpd` | Bring-up console / web UI |

### VoIP seams

1. **Audio taps.** RTP reads a line's microphone audio from `audio` (`audio_rx_read`,
   or the echo-cancelled `audio_aec_read`) and writes the far end's audio into the
   line's external stream (`audio_ext_write_downlink`), converting between
   8 kHz G.711 and the codec's 48 kHz.
2. **One call API.** `telephony` places and ends calls with `voip_dial()` (through
   a SIP server), `voip_dial_local()` (straight to another board), `voip_answer()`
   and `voip_hangup()`.
3. **Events into the same state machine.** `voip` reports `INCOMING`, `ANSWERED`
   and `CLOSED`, which `telephony` treats like the matching events of a local call.

So the whole telephony stack (dialing, tones, ring, dial plan) works with no
network at all, and the board is a standalone two-phone exchange until VoIP is
used.

---

## 4. Bring-up console

The serial console's hardware-test commands are listed in
[Flash & test](flashing.md#6-test-the-hardware); type `help` on the console
for all of them. Toolchain and build-system detail:
[`firmware/README.md`](https://github.com/sdylewski/Analog-Redial/blob/main/firmware/README.md).
