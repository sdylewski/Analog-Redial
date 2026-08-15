# Echo reduction — test log

Live tracking doc for the 2026-08-15 echo debugging session. Background,
tooling, and everything ruled out before today: see
[`ECHO_DEBUGGING.md`](ECHO_DEBUGGING.md). This doc is the results log —
Scott describes each test as he runs it, results get filled in here so we
have something to come back to.

## Reference: what "normal" telephone levels look like

TIA-810/TIA-920 (digital/VoIP-era, supersedes the older analog TIA-810)
target **Send Loudness Rating (SLR) = 8±3 dB**, **Receive Loudness Rating
(RLR) = 2±3 dB**, per ITU-T P.79's frequency-weighted measurement method.
ITU-T G.712's standard test reference is **-10 dBm0** (0 dBm0 as an
alternate), **1020 Hz** tone — the real-world convention `echopulse`'s
1kHz default already happens to match.

Caveat: properly *measuring* SLR/RLR needs calibrated acoustic test gear
(artificial ear/mouth, IEC 60318/60268-3) we don't have. Useful here as a
conceptual reference and relative comparison point, not an exact target.

**Gain-adjustment order** (standard practice, and why): tune **receive**
(`spkamp`) for comfortable listening first, leave it there. Then use
**send** (`micamp`) as the primary echo-control lever — reducing mic gain
doesn't change what you hear locally, only how much of anything that made
it into the mic (including a leaked echo) gets sent back out.

## Commands available

| Command | Purpose |
|---|---|
| `micamp <line> <left 0-8> <right 0-8>` | Mic/ADC gain, live, 3dB/step, 0=0dB..8=+24dB |
| `spkamp <line> <left 0-63> <right 0-63>` | Earpiece/DAC output gain, live, 1dB/step, 33=0dB |
| `echopulse <line> <on\|off> [period_ms]` | Repeating 1kHz/10ms burst, timestamped |
| `echosweep <line> <on\|off> [period_ms]` | Cycles 9 tones 300-3400Hz, timestamped |
| `record <ms> [line]` | Raw mic capture to storage (not safe concurrently with an active call's RTP -- see `ECHO_DEBUGGING.md`) |

Numbers: MaxPhone = `1055`, DualPhone line 1 = `1031`, line 2 = `1032`.
Dial as `555` + the number (e.g. `5551055`).

---

## Test 1 — Reproduce the echo physically

**Goal:** confirm baseline — echo is present, note conditions.

**Result:** _pending_

## Test 2 — Handset geometry / hand-over-mic

**Goal:** try different ways of holding the phone, covering the mic by
hand, etc., and see if the *other* end hears a difference. Tests whether
the leak path is acoustic and local to the handset (mic pickup of nearby
sound) vs. something upstream of the mic entirely.

**Result:** _pending_

## Test 3 — Physical phone vs. Linphone

**Goal:** one end physical phone, other end Linphone (softphone, no
handset hybrid at all). If the echo still happens, that isolates it away
from "two real handsets interacting" and toward one specific end's
hardware/config. Ideally repeat in different rooms (per the
garage/upstairs finding that ruled out room-acoustic coupling).

**Result:** _pending_

## Test 4 — Gain adjustment during a live echoing call

**Goal:** reproduce the echo, then live-adjust `spkamp` and/or `micamp`
mid-call (no hangup needed, instant I2C register writes) and see what
level(s) reduce it, and by how much. Needs two consoles (one per board)
since gain has to be adjusted on whichever end is *causing* what the
other end hears — try both ends independently if unsure which.

**Result:** _pending_

## Test 5 — Tone sweep

**Goal:** run `echosweep` during a call and listen for whether some
frequencies echo more than others (tests the "handset acoustic cavity has
resonances" theory — a single 1kHz probe may have landed in a quiet part
of that response curve, unlike broadband real speech).

**Result:** _pending_

## Test 6 — Record for delay measurement

**Goal:** capture a test (packet capture on the Asterisk server, same
method as before) specifically to calculate the actual echo delay,
however it ends up getting reproduced most reliably above. Precise number
matters for two things: confirming/refuting the "~100ms each way, ~200ms
round trip" network-latency theory, and — if we do end up building an
echo canceller — sizing the filter correctly.

**Result:** _pending_

---

## Summary (fill in once tests are done)

_What we learned, and what it points to next._
