# Answering machine — background

Implements the "Answering machine" per-line card on Settings -> Hardware
(auto-answer after N rings, play a greeting, beep, record the caller).
This is the engineering-journal writeup: the design as agreed with the
user, and — more importantly — a real audio-routing gap this feature
uncovered mid-implementation that isn't obvious from reading telephony.c
alone.

## Design, as agreed

- Per line: enabled checkbox, rings-before-answer (default 4, matching the
  common consumer-answering-machine convention — roughly 20-30s of
  ringing before pickup, and this board's cadence is 2s on / 4s off = 6s/
  cycle, so 4 rings ≈ 24s), max caller-message length (default 30s), and
  (2-line boards only) a separate "also answer unanswered calls from other
  line" checkbox, default off.
- Greeting: recorded over the phone (dial 212, speak after the beep, hang
  up or pause ~8s to save) — reuses the exact same record-prompt/beep/
  record machinery as 211, just with a fixed per-line filename
  (`am_greeting_l<N>.wav`, overwrites directly like a real answering
  machine) instead of a timestamped archive entry. Reviewed via the web
  UI's own playback (`<audio>` element, same pattern already used
  elsewhere) rather than a phone-side confirm loop — deliberately simpler,
  since the file's already sitting there to listen to once it's recorded.
- Caller's message: same shape (prompt/greeting -> beep -> record), capped
  at the per-line max length or ~8s of silence, saved as
  `rec_l<N>_am_<timestamp>.wav` -- "rec_" prefix kept (not "vm_" or
  similar) so storage.c's `is_protected_asset()` keeps treating it as an
  ordinary, evictable recording, same as any 211 archive; "am_" tags it so
  it's visually distinguishable in the Recordings list, which also already
  sorts newest-first.
- Barge-in: picking up the real handset at any point (during the greeting,
  the beep, or the recording itself) takes the call live instead, and the
  partial capture is just left in place -- explicitly OK'd by the user
  ("ok to ignore/delete the recording") rather than built out as an actual
  deletion path.
- No VoIP-specific language anywhere in the UI, per the user's request for
  the low-power feature's UI and carried over here for consistency -- the
  local-vs-VoIP distinction is entirely internal.

## The real discovery: making a VoIP caller actually hear anything

This is the part worth reading carefully before touching this code again.

Every existing use of `player_play()`/`audio_tx_tone()` in this codebase
(dial-code clips, operator help, 211's own prompt/beep) plays audio to the
**local handset's own earpiece** -- that's what `audio_tx_*()` has always
meant here. Nothing before this feature ever needed a *remote* VoIP party
to hear synthesized/local audio, because every existing feature assumes a
real human is physically on the line generating (or listening to) that
audio themselves.

The answering machine breaks that assumption for the first time: on an
auto-answered VoIP call, nobody is physically holding the handset, so:

1. **The greeting has to reach the caller, not a local speaker.** Outgoing
   RTP audio (`rtp.c`'s `tx_task`) is built from `audio_rx_read()` -- the
   real mic ADC input. With no one at the handset, that's just room
   silence. Playing the greeting via `player_play()` alone (which only
   drives `audio_tx_external()`/TX-toward-the-local-DAC) would never reach
   the caller at all.

   Fixed with a new primitive, `audio_set_uplink_inject(line, bool)`
   (audio.c/.h): substitutes this line's own current TX output (whatever
   `audio_tx_*()` last selected -- tone, external/player, bridge) for its
   mic input on the uplink side, scoped to one line. This is the exact
   same TX-becomes-RX swap `audio_set_test_loopback()` already does for
   bring-up (`audio_task`'s RX-processing block, `s_rx_mono[line]` vs.
   `txL`/`txR`) -- test_loopback just does it globally for both lines at
   once, which isn't safe to reuse in production on a 2-line board where
   the other line could be on a real call. `auto_answer()` (telephony.c)
   turns this on for the VoIP case only, and it's turned back off
   wherever the machine session ends (barge-in, `end_call()`).

2. **The caller's voice has to be capturable independent of what we're
   currently sending.** The obvious tap point, `audio_add_downlink_tap()`
   (already existed, used by the bring-up `record_downlink` console
   command), observes the *muxed TX output* (`txL`/`txR`, post
   `fill_tx_line()`) -- i.e. exactly what this line is currently
   transmitting. That's fine for bring-up, but wrong here: the instant the
   recording phase starts, this line's own TX is silenced (so the caller
   hears quiet, not a looping beep) -- and a silenced TX means
   `audio_add_downlink_tap()` would observe silence too, not the caller's
   actual incoming audio, even though `rtp.c`'s `rx_task` is still
   decoding and delivering it the whole time via `audio_ext_write()`.

   Fixed with a second new primitive, `audio_add_extwrite_tap()`
   (audio.c/.h): taps `audio_ext_write()`'s own input directly, before any
   muxing -- sees exactly what's written there (decoded VoIP RTP from
   `rtp.c`, or a locally played clip from `player.c`, whichever is
   currently feeding it) regardless of what this line's `src` selector is
   doing. `recorder_start_downlink()` (recorder.c) uses this, not
   `audio_add_downlink_tap()`, for exactly this reason -- see that
   function's own doc comment for the full trace. Also relevant: since
   `player.c` and `rtp.c` share the *same* `audio_ext_write()`/`ext`
   buffer per line, playing the greeting and receiving the caller's own
   background audio *do* collide/interleave in that shared buffer for the
   few seconds the greeting itself is playing -- harmless in practice
   (nothing is being recorded yet at that point; recording only starts
   after the beep, by which point the greeting's `player_play()` has
   already finished and stopped writing), but it does mean a caller who's
   making noise *during* the greeting might hear a slightly garbled
   version of it. Known, accepted limitation -- not worth solving for v1.

Neither of these needed for the **local-peer** case: two lines on the same
board talk to each other via `audio_tx_bridge()`/`s_rx_mono[]` directly,
no RTP encode/decode boundary, no shared `ext` buffer, no muxing gap. The
calling line's own mic and earpiece work exactly like any other local
recording (211, 212) -- `start_recording_voicemail()` picks
`recorder_start()` vs. `recorder_start_downlink()` based on `peer ==
PEER_VOIP`, and everything above is inert for the local case.

## Where the "called" line goes during a local-peer auto-answer

The calling line does all the actual work (plays the greeting, records the
message) -- but the **called** line deliberately does *not* transition to
idle when the machine picks up; it stays in `TEL_RINGING` (bell silenced,
`peer` still pointing at the caller). This is what makes barge-in work: a
real pickup on the called line still lands in `on_offhook()`'s normal
`TEL_RINGING`/local-peer branch, which now checks whether the peer is
mid-answering-machine-activity and, if so, aborts it and bridges live
instead of the plain `connect_bridge()` it would otherwise do. Two
sub-cases there, driven by whether `player_stop()` reliably fires its done
callback (it doesn't -- confirmed by reading `player.c`'s `finish()`,
whose `call_cb` is tied to whether playback actually completed, unlike
`recorder.c`'s callback which fires unconditionally even after an explicit
`recorder_stop()`): still-playing-the-greeting bridges immediately
(synchronously, nothing to wait for), already-recording defers the bridge
to `IN_REC_DONE` (via the `am_bargein` flag) since that event is reliable.

## Ring counting

Didn't exist before this feature -- `ag1170_ring_start()` just drives the
F/R cadence, nothing previously counted cycles. `ag1170_ring_cadence_ms()`
(new getter) exposes the configured on/off durations so telephony.c can
derive "N full ring cycles have elapsed" from `ring_started_us` (set at
both `ag1170_ring_start()` call sites) without duplicating the cadence
constants. Deliberately "N *full* cycles elapsed," not "currently on ring
N" -- simpler, no off-by-one ambiguity about whether a ring in progress
counts.

## What's deliberately out of scope (v1)

- No dial-in remote playback of messages (explicitly scoped out by the
  user -- "Web UI only for message playback for now").
- No deletion of a barge-in-interrupted partial recording -- left in
  place, same as any short/quiet recording already is.
- No TTS auto-generation of a default greeting text -- the prompt-text
  field pattern (`gen_prompt.py --sync`) already exists for this via the
  same mechanism Operator/211 use; nothing answering-machine-specific was
  added for it since the phone-recorded flow covers the primary path.
