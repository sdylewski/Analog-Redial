# Design Verification Test (DVT) Protocol

End-to-end verification of the ESP32-C6 rotary phone gateway's features. Most
tests are **automated** by [`verify.py`](verify.py) (host-side, drives the
device's HTTP API + optionally the serial console). A few require a **human**
(lift a real handset, listen to a tone) and are called out as `MANUAL`.

## How the automation works

Two device-side facilities make hands-free testing possible (both off in
normal use, toggled per-run by the harness — see Settings → Diagnostics or
`POST /test/*`):

- **Injection test mode** (`telephony_test_*`): simulates a handset —
  off-hook, hang-up, and dialed digits — fed into the exact same telephony
  state machine a real handset drives. The injected hook state also shadows
  the real SLIC, so post-clip / post-record transitions behave identically to
  a physical phone.
- **Audio TX→RX loopback** (`audio_set_test_loopback`): feeds synthesized TX
  audio (dial tone, DTMF, a playing clip) back into the RX chain (level meter
  + DTMF detector), so a test can confirm audio is actually produced and
  detectable without a handset. This is a DSP-domain loopback; it does **not**
  exercise the analog codec I/O (that's what the mic→speaker `loopback`
  command + a listen are for).

State is observed via `GET /api/status` (per-line `state`, `last_number`,
`rx_level`; plus `test_mode`, `test_loopback`, `log_level`).

## Running it

```bash
cd firmware/test
./verify.py --host 192.168.0.233                 # HTTP-only tests
./verify.py --host 192.168.0.233 --serial /dev/cu.usbmodem101   # + console tests
./verify.py --host 192.168.0.233 --serial auto --physical       # + ring/SLIC
```

- `--serial` adds tests that need the console (DTMF gen→detect, tone frequency
  checks, `sd_test`). Opening the serial port resets the board; the harness
  waits for it to re-join WiFi before starting.
- `--physical` opts into actions that energize the line (ring, SLIC drive).
- `--only <name>` / `--skip <name>` to filter; `-v` for per-step detail.

The harness prints a PASS/FAIL line per test and a summary; exit code is the
number of failures.

---

## Test matrix

| # | Test | Transport | What it asserts |
|---|------|-----------|-----------------|
| 1 | Connectivity | HTTP | `/` returns 200; `/api/status` parses; board name/serial present |
| 2 | Log level control | HTTP | set `debug` → `/api/status.log_level==debug`; restore `info` |
| 3 | Storage round-trip | HTTP | upload a WAV via `/media/<n>`, list shows it, download matches bytes, delete removes it |
| 4 | Browser upload + convert | HTTP | POST 44.1k stereo WAV to `/operator_upload`; download back is 8 kHz mono 16-bit |
| 5 | Dial: operator (0) | HTTP+inject | off-hook → dial `0` → line state `PLAYING`; on-hook → `IDLE` |
| 6 | Dial: clip codes | HTTP+inject | each Dial-Codes entry: dialing its code → `PLAYING` (file present) or confirm-tone fallback |
| 7 | Dial: record (211) | HTTP+inject | dial `211` → state `RECORDING`; on-hook → recording finalized (`rec_*` in storage) |
| 8 | Dial: readback toggle (921/922) | HTTP+inject | dialing sets the mode; `/api/status` / dial guide reflect it |
| 9 | Dial: wrong number | HTTP+inject | dial an unassigned 3-digit code → state `REORDER` |
| 10 | Dial tone audio | HTTP+inject+loopback | off-hook under loopback → `rx_level` rises above idle floor |
| 11 | Clip playback audio | HTTP+inject+loopback | dial operator under loopback → `rx_level` rises while `PLAYING` |
| 12 | DTMF gen → detect | serial+loopback | `dtmf_gen 519` under loopback → detector reports those digits (`tonemon`) |
| 13 | Pulse/hook debug logging | serial | at `debug`, injected/real hook + digit events emit timing logs |
| 14 | microSD write/read/verify | HTTP or serial | `/test/sd` → `sd: ok` (or `sd_test` console exit 0) |
| 15 | Regional tones | serial | set language, `dialtone`, confirm selected region freq/cadence in log | 
| 16 | VoIP config + script | HTTP | `/voip_set` persists; `/voip_server_script` downloads valid bash (`bash -n`) |
| 17 | Ring cadence | serial+`--physical` | `ring 0 2` drives RM/FR; `MANUAL` confirm the bell |
| 18 | SLIC pin control | serial+`--physical` | `slic 0 fr 1/0` toggles; `MANUAL`/scope confirm |
| 19 | Handset off-hook (real) | `MANUAL` | lift the real handset → dial tone; dial a code by hand |

Tests 17–19 touch real hardware / require a person; the rest are automated.

## Interpreting results

- **PASS** — assertion held.
- **FAIL** — assertion failed; the line shows expected vs. actual. Investigate
  before shipping.
- **SKIP** — prerequisite missing (`--serial` not given, file not present,
  `--physical` not set). Not a failure, but that coverage didn't run.

## Notes / limits

- Loopback tests verify the DSP chain (synthesis → detect), not the analog
  codec path or the SLIC/handset audio — verify those by ear with the
  mic→speaker `loopback` command and a real handset (test 19).
- Dialing tests use injection, which drives the real FSM/dialplan/player, so a
  PASS means the software path works end to end; the only thing simulated is
  the handset's hook + pulses.
- Clip-playback tests need the clip's WAV actually in storage. Fresh boards
  ship only `operator.wav`; other clips report a confirm-tone fallback (still a
  valid, tested path) until you upload audio for them.
