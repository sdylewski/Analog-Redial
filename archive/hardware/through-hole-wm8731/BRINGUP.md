# Through-hole board bring-up (serial console)

Step-by-step console walkthrough for the original through-hole reference
board (ESP32-C6-DevKitM-1 + WM8731 + AG1170). This used to live in
`firmware/README.md`; moved here once SMD4 became the board anyone starts
from — the modern, board-agnostic equivalent is
[`../../../firmware/bringup/README.md`](../../../firmware/bringup/README.md)'s
`flash_board.py --setup` and `bringup_checklist.py`, which run the same kind
of sequence without typing each command by hand. This doc is for anyone
still bringing up a THT board specifically, where the commands below
(`wm8731_init`, `wm8731_dump`, `wm8731_bypass`) are WM8731-only — they don't
exist on an SMD/ES8388 board.

After boot you get a prompt named after the board (e.g. `telephono>`). The full
telephony stack is already running concurrently. Type `help` for the full
list. Recommended order:

| Step | Command | Pass criteria |
|------|---------|---------------|
| 1. Power rails | (multimeter) | 3.3 V, 1.8 V (DCVDD via U3), 5 V to SLICs |
| 2. I²C bus | `i2c_scan` | Reports `0x1A  <- WM8731` |
| 3. Codec config | `wm8731_init` then `wm8731_dump` | `init: ESP_OK`; sane registers |
| 4. Tone out (DAC) | `tone 1000 1500 0` | 1 kHz heard on Line 0 handset; `tone 1000 1500 1` for Line 1 |
| 5. Call-progress | `dialtone 0`, `ringback 0`, `busy 0`, `silence 0` | Correct cadences |
| 6. Capture (ADC) | `capture 4000` (talk/whistle into handset mic) | `rms` rises with sound |
| 7. Loopback | `loopback 0 1` (talk) → `loopback 0 0` | Hear your own voice (codec + I²S round trip) |
| 8. Codec bypass | `wm8731_bypass 1` | Analog line-in straight to out (isolates DSP from analog) |
| 9. SLIC power | `slic 0 pd 1` (run), `status` | Line powered |
| 10. Ring | `ring 0 6` | Bell rings ~2 s on / 4 s off |
| 11. Hook/pulse | `hookmon 15` (lift handset, dial digits) | SHK toggles; pulse breaks visible |
| 12. DTMF gen | `dtmf_gen 0123456789 0` | Tones audible / detectable |
| 13. DTMF detect | inject DTMF on the line; watch logs / `status` | Digits decoded |
| 14. Dialing end-to-end | lift handset → dial `369`/`842`/codes | Dial tone, digit collection, action logs |

No PCB yet? You can still flash a bare DevKitM-1 and run the console — the
hardware-dependent commands fail gracefully (e.g. `i2c_scan` finds nothing,
`hookmon` reads floating pins). The DSP commands (tones, DTMF gen) and the
state machine run regardless.
