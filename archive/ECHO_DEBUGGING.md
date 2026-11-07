# Debugging the VoIP call echo

## Symptom

Medium-strength echo of your own voice on VoIP calls, ~500-750ms delay.
Observed on both boards (MaxPhone, single-line; DualPhone, 2-line), and in
both directions (board-to-board and board-to-Linphone) — so it's not a
"one bad SLIC" thing, and not the far end's audio path either.

## Ruled out so far (2026-08)

- **WiFi**: RSSI was briefly suspected (DualPhone measured -90 dBm at one
  point, MaxPhone -65 dBm), but the echo persisted calling board-to-Linphone
  with everything sitting right next to the router. Not the cause.
- **RTP jitter buffer**: none exists (`firmware/components/voip/rtp.c`) —
  packets are decoded and played the instant they arrive, ~20-40ms of
  inherent packetization delay at most.
- **Internal audio stream buffers** (`firmware/components/audio/audio.c`,
  `xStreamBufferCreate(s_frame * 8 * sizeof(int16_t), ...)`): capacity caps
  out around 80ms. Even fully backed up, can't account for 500-750ms.
- **I2S DMA depth**: using stock small ESP-IDF defaults, not an explicit
  large buffer.

None of the three obvious software layers account for a delay that long.
Leading theory: real hybrid/line echo from the AG1171 SLIC's 2-wire
tip/ring loop (any analog phone-line interface leaks some of what it's
*playing* back into what it's *picking up* — this firmware has no echo
canceller yet) — but a *simple* hybrid leak should be near-instant (µs to
a couple ms), not half a second. The "medium strength" matches a hybrid
leak (plausibly worse on the cheaper AG1171 clones); the long *delay*
doesn't fit that story on its own and needs to be measured, not guessed.

## The tool: `echopulse`

Added to the bring-up console (`firmware/components/bringup/bringup.c`)
specifically for this. Plays a short, distinctive 1kHz/10ms tone burst on
a fixed period, logging the exact timestamp of each one:

```
echopulse <line> <on|off> [period_ms, default 2000]
```

```
phone> echopulse 0 on
echopulse line 0: started, 1kHz/10ms burst every 2000 ms
echopulse line 0: PULSE at t=123456 ms
echopulse line 0: PULSE at t=125456 ms
...
phone> echopulse 0 off
```

It goes out through the **real DAC → ES8388 → AG1171 → tip/ring** path —
deliberately *not* through the firmware's digital `test_loopback` tap
(`audio_set_test_loopback()`), which explicitly bypasses the analog codec
and SLIC. The whole point is to exercise the real hybrid. It also runs
independent of hook/call state — it's a raw audio-layer primitive — so you
can test with the handset on-hook, off-hook, mid-call, whatever's useful.

### Test 1 — local only, oscilloscope (do this first)

No call, no second board needed. This isolates the hardware question
completely: does *this board's own hybrid* leak, and by how much delay?

1. Probe your existing post-codec tap point.
2. If you have (or can add) a second probe point on the mic/ADC-input side
   of the same board, watch for the same pulse reappearing there.
3. `echopulse 0 on`
4. Compare: does the pulse show up on the mic-input trace? How long after
   the DAC-output trace shows it?

This is the cleanest test because it removes the network and the far end
entirely — if you see a delayed echo of the pulse locally, the delay is
happening *on this board*, full stop.

### Test 2 — over a real call

1. Place a call (board-to-board, or board-to-Linphone).
2. `echopulse 0 on` on the sending side.
3. Far end: confirm the beep arrives every ~2s as expected.
4. Sending side: listen (or scope, if you can get a probe on the earpiece
   output while on a call) for a delayed, quieter *repeat* of your own
   beep coming back. Its arrival time relative to the logged
   `PULSE at t=...` line is the measured round-trip echo delay.

### Companion: `capture`

Already existed before this session. Prints RMS mic input level for both
lines every 200ms:

```
capture [ms]
```

Run this in a second monitor session alongside `echopulse` for a rough
software-side cross-check without the scope — if mic level spikes each
time a pulse should be echoing back, that corroborates a real leak and
roughly brackets the timing (200ms resolution, plenty fine for a
500-750ms delay).

## If it turns out to be real hybrid echo, not a latency bug

Line echo cancellation (LEC) is implementable — adaptive NLMS filter,
standard technique in every analog-to-VoIP gateway. The filter length has
to cover the full delay it's cancelling: a normal 2-wire hybrid only needs
~8-16ms of tail (cheap, comparable cost to the DTMF Goertzel detector
already running). If the delay genuinely turns out to be hundreds of ms
even in the *local-only* test, that's acoustic-echo-canceller-scale
filtering, not line-echo-canceller scale — 40-60x more taps, real CPU cost
on a chip with no hardware FPU. Measure first; that number decides which
of these two very different implementations is actually needed.

## See also

- [`ECHO_REDUCTION_TESTS.md`](ECHO_REDUCTION_TESTS.md) — live test log:
  results of everything tried, loudness-standard reference values, and
  what's left to test
- `firmware/components/bringup/bringup.c` — `cmd_echopulse`, `cmd_capture`,
  `cmd_echosweep`, `cmd_micamp`, `cmd_spkamp`
- `firmware/bringup/README.md` — full console command reference
- `hardware/smd2.md` — SLIC levels, SHK dividers, the DTMF-clipping rework

## 2026-11-06: canceller fixes

Echo heard at maxphone (touch-tone phone) on calls with dualphone2 (rotary
phone), little the other way; dualphone2's Measure gave 21 ms, and applying
it plus enabling the canceller made the audio more garbled. Found:

- **Measure reported the echo's peak, Apply used it as the window start.**
  The 10 ms burst's echo peaks up to ~10 ms after it starts, so 21 ms put the
  40 ms window past the start of the echo. Measure now suggests peak − 15 ms
  (`anchor_ms`, 6 ms for that reading) and reports the echo loss (`loss_db`).
- **Weight updates truncated toward minus infinity**, so every weight drifted
  negative. In `test_aec.c` the old code made a 20 dB-down echo 7.7 dB
  *louder*. Now rounded.
- **Double-talk didn't freeze adaptation** (the flag was only used to decay
  weights 1/64 per block afterwards), and its fixed threshold assumed a
  -20 dB path: a 6 dB-down path (a poorly matched phone) read as near-end
  speech all the time. Now decided before adapting, against the filter's own
  path-gain estimate, with 40 ms hangover.
- Added: output never louder than input (restores the last good weights after
  3 bad blocks), and ~10 dB residual echo suppression while only the far end talks.

Host test results (old → new ERLE): 20 dB path −7.7 → 34 dB, 6 dB path
8 → 45 dB; near-end voice through double-talk ±0.1 dB. Not yet tried on a call.

Asymmetry: the echo maxphone hears is made at dualphone2's end (its SLIC's
2-to-4-wire leak plus the handset). Untested theory: the rotary set's
impedance (carbon transmitter, induction coil) matches the AG1171 worse than
the touch-tone set does. Swapping the two phones between boards would show
whether the echo follows the phone.
