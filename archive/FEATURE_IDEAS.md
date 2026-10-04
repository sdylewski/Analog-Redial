# Future feature ideas (not yet built)

Open brainstorm list -- nothing here is committed to, just worth remembering
next time there's bandwidth for new features. Add to this rather than losing
ideas in chat history.

## In-call DTMF feature codes

Idea (2026-09-30): since DTMF is detected continuously, could we let someone
dial a code *during* an active call to change something (toggle a setting,
trigger an effect, etc.), not just while collecting a number to dial?

**Detection already works today, mid-call, for free.** `dtmf_tap()`
(`telephony.c`) runs the Goertzel detector on every audio block whenever the
line is off-hook -- independent of dial state -- and already timestamps
digits via `telephony_last_dtmf()` even during `TEL_INCALL`. It's tapped on
the uplink side (`audio_set_rx_tap()`), i.e. the local handset's own mic/line
signal heading out, not the downlink audio coming back from the far end --
so it's specifically hearing tones *this* phone generates, unaffected by
whatever VoIP/Bluetooth audio is also flowing.

**The gap:** `process_digit()` currently drops every digit unless
`state == TEL_DIALTONE || state == TEL_DIALING`:

```c
if (st != TEL_DIALTONE && st != TEL_DIALING) {
    return; /* ignore digits unless we're expecting them */
}
```

To use mid-call digits, add a parallel path gated on `TEL_INCALL` (maybe
`TEL_RINGBACK` too) with its own small collector -- the existing
`dialplan_feed()`/`dialplan_collector_reset()` machinery is built around
dialing-a-number semantics (timeouts, prefix matching) and isn't a great fit
for a short in-call code like `*1#`. A dedicated little buffer (watch for
`*`, collect until `#` or a timeout, match a small code table) mirrors the
existing `process_digit()` -> `dialplan_feed()` split.

**Caveat worth remembering before building this:** the SLIC passes the local
phone's audio straight through to the line in real time, so by the moment
the ESP32 detects the tone, it has already gone out over the live call too --
the far end (VoIP peer, or whoever's on the Bluetooth-paired call) will hear
the beep. Not fixable after the fact on a 2-wire analog line; it's the same
tradeoff real telco features (flash-hook transfer, `*72`, etc.) have always
had.

## Voice effects -- BUILT 2026-10-04

Done: `components/audio/voice_fx.c`. During any call, a touch-tone (or
rotary-dialled) 5/6/7/8/9 sets Dalek / chipmunk / Vader / 8-bit / robot, 0
turns it off, and both phones hear it whoever presses (`in_call_key()` in
`telephony.c`). 322-324 start a call with chipmunk/Vader/Dalek. Dalek, 8-bit
and robot add no delay; chipmunk and Vader use a 30 ms delay-line pitch
shifter (~15 ms).

Still open here:
- VoIP: a key pressed by the far end isn't detected (only local phones'
  digits are), and keys pressed on this end also reach the far end as tones
  -- so an automated phone menu on a VoIP call will see them, and pressing 5
  for a menu option will also turn on Dalek. A `*` prefix would avoid that if
  it turns out to matter.
- More effects that add no delay: tremolo, a "radio" band-pass with
  crackle. Vader could also get a breathing loop between phrases.

## Other ideas that fall out of having mid-call DTMF codes

- **DTMF soundboard**: trigger a clip (`player.c`) into the local TX path
  mid-call -- reuses existing clip-playback infra (`clip_done_cb`, etc.)
  almost as-is.
- **Mute/privacy toggle**: stop sending local audio to the peer without
  hanging up -- straightforward given `audio_tx_*` is already abstracted
  per-line.
- **Random per-call voice effect**: pick an `FX_*` effect automatically per
  call instead of needing to dial a specific code for it, once voice effects
  themselves exist.
- **In-call recording toggle**: capture an active 2-way call on demand,
  reusing the answering-machine recorder (`REC_VOICEMAIL` path) -- flagged
  here mainly because call recording has real two-party-consent legal
  implications depending on jurisdiction, worth thinking through before
  building, not just a technical question.

## More spoken-prompt variables

Prompt text expands `{{boardname}}` (case-insensitive) **once, when the audio
is generated**: `gen_prompt.py --sync` fetches the text with the variable
already filled in (`expand_prompt_vars()` in `webui.c`), synthesizes it, and
uploads an ordinary WAV. The board never assembles speech itself. Keep any
new variable to that same rule -- something that's fixed at generation time
(board name, a line's extension, the owner's name).

Easy additions under that rule: `{{extension}}` (this line's VoIP number),
`{{lines}}` (how many phones are connected).

> **Big caveat -- not planned:** values that change after generation
> (`{{time}}`, `{{day}}`, `{{messages}}`) would need the board to stitch
> speech together at play time from pre-recorded word clips ("you have" +
> "three" + "messages"). That always sounds choppy next to a sentence
> synthesized in one go, so it isn't worth doing until there's a way to
> produce natural-sounding speech on the fly. Revisit only if that changes.

Also worth adding: mark a prompt "pending" again when the board is renamed,
since its audio still says the old name.
