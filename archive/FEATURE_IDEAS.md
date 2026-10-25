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

## Voice effects -- BUILT

`components/audio/voice_fx.c`. During any call, a touch-tone (or
rotary-dialled) 1-7 sets Dalek / chipmunk / Vader / 8-bit / robot / tremolo /
distortion and 0 turns it off; both phones hear it whoever presses
(`in_call_key()` in `telephony.c`). 322-324 start a call with
chipmunk/Vader/Dalek. Settings -> Phone Settings has an on/off switch.

Still open:
- If both ends of a VoIP call are Analog Redial boards and *both* press a
  key, both apply their effect, so each voice gets it twice.
- A far-end phone that isn't an Analog Redial can't trigger effects (its
  tones arrive on the downlink, which isn't scanned for digits).
- Vader could get a breathing loop between phrases.

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

## Detect the Bluetooth module type at boot

Idea (2026-10-09): fit a BM64 or BK3266 and let the firmware work out which
it is, instead of asking in Settings -> Hardware (the manual choice stays as
an override).

**Looks feasible**, because the two speak completely different protocols on
the same pins and both answer a harmless query:

| | BK3266 | BM64 |
|--|--|--|
| Baud | 9600 | 115200 |
| Framing | `52 42 23 LEN CMD DATA CRC 23 42 52` | `AA 00 LEN OP DATA CHK` |
| Powers on | by itself at boot (may send a `0x64` boot frame) | only after an MFB high pulse |
| Probe | version query `52 42 23 04 01 14 19 23 42 52` | `GET_UART_VERSION` |

**Order matters.** Probe the BK3266 first and passively: listen at 9600 for
a boot frame, send the version query, touch no pins. Only if nothing
answers, run the BM64 power-on (MFB high 400 ms per the datasheet) and send
`GET_UART_VERSION` at 115200. Reversed, the MFB pulse could turn a BK3266
off or reconnect it (WAKEUP's behaviour is an open question), and holding
MFB high at power-up wipes a BM64's pairings.

Things to settle: do it in the background task, not in the boot path (a
board with nothing fitted would spend a few seconds failing both probes);
cache the result in NVS and show it in Settings -> Bluetooth ("detected:
BK3266"); a text-AT BK3266 answers `BT+GNM` with `NA_...` rather than binary
frames, so a third probe is possible; and decide what happens when the
manual setting and the detection disagree (suggest: warn, trust the
detection). Needs a real module of each kind to validate.

## Catch a wrong SMD5 jumper (JP2/JP3) at boot

Idea (2026-10-09): a clone MCU module with the jumpers left at default
leaves the codec without a clock and the card-detect switch unread, with no
error anywhere (see `archive/hardware/smd5.md`).

There is **no way to see the codec's MCLK pin or the card-detect net from
the MCU directly**: each net has only the one MCU pad on it, and the codec
MCLK input is high-impedance. Two indirect checks do work:

1. **Codec clock, by data.** With MCLK missing the ES8388's I2S output is
   dead. After codec init, read ~20 ms of ADC samples; a stream that never
   changes (all zeros) while the codec reports powered-up means no clock.
   Needs validating on a real board to be sure a *working* quiet codec's
   noise floor is reliably non-constant. Could be a self-test row and a log
   warning "no data from codec: check JP3".
2. **Card detect, by SPI.** The card talks over SPI regardless of the CD
   pin. If the board is set to microSD and CD reads "empty", try one SPI
   mount: if it succeeds, the card is there and CD is not wired to the pin
   the firmware reads, which is exactly a wrong JP2. (Today the firmware
   refuses to attempt a mount when CD reads empty.) Do it once at boot or
   from a "Check microSD" button, not repeatedly.

**Optional step further:** if check 1 fails on an SMD5 build, move MCLK to the
clone's GPIO12 at runtime (what the `SMD5_CHEAP` build does at compile time)
so one firmware self-corrects, at the cost of the USB console on that boot.


## The four spare pins as inputs too (switch, door contact, doorbell)

Idea (2026-10-14): GPIO18-21 on J5 are ordinary pins, so in hardware they
could be inputs as well as outputs: a wall switch, a door or window contact,
a doorbell button. Today they are **outputs only** (`auxpins.h`: "as
outputs"; a dial code drives one high or toggles it). The overview diagrams
therefore say "4 pin outputs"; rename to "4 spare I/O pins" only once this
exists.

What it would need:

- **Per-pin mode** (output / input, and pull-up / pull-down for an input) in
  the Pin outputs card, stored with the pin settings. All four pins are
  floating inputs after boot today, so a pull has to be a deliberate choice.
- **Debounce**: ~50 ms settle in a small poll or a GPIO interrupt that wakes
  a task, so a bouncy switch doesn't send a burst.
- **A `pin_input` event** (`pin`, high or low) sent through `webhook_event()`,
  which feeds the webhook and MQTT together, as dial and hook events do.
- **Home Assistant**: one `binary_sensor` per input pin in the MQTT discovery
  (the output pins already get a button each), plus the state topic.
- **Optional, more design**: a pin change triggers a dial-code-style action
  on the phone (doorbell input rings the bell, door contact plays a clip).
- **Electrical**: 3.3 V logic, not 5 V tolerant, no protection on the
  header. A 5 V sensor or a long cable needs a divider or level shifter and
  a series resistor; say so on the card, as the output note does for relays.
- **Same conditions as outputs**: only live on a one-line board with no
  microSD and the J5 shunts out (`auxpins_usable()`); otherwise the pins
  belong to line 1's SLIC or the card.

Scope is about the same as the pin outputs work: a small `auxpins` change,
one card option, a few lines of discovery, and a test with a real switch on
J5. Not started.

## Calling out through a phone service provider (SIP trunk)

Idea, not built (2026-10-24). A "Phone service provider" card so a line can
register with a commercial SIP provider and dial real phone numbers, without
running Asterisk.

- **Google Voice can't be done.** Google dropped open XMPP access in 2014, gave
  only Obihai a proprietary SIP variant, and since Obihai's 200-series ended in
  2021 no device from any vendor connects to consumer Google Voice. Workarounds:
  forward Google Voice to a number on a SIP provider (incoming only), or place the
  call on a Bluetooth-paired phone.
- **Standard SIP providers look feasible**, e.g. VoIP.ms (its wiki: UDP 5060,
  digest auth, G.711 u-law). The UA already does UDP registration with MD5 digest
  auth and refresh, PCMU, hostname servers and `rport`.
- **What's missing:** a call-out dial rule (10 digits, or 1 + 10, which is the
  Bluetooth dial-out form, so one or the other per line); a separate auth username,
  E.164 number formatting (`+1...`), caller ID, an optional outbound proxy;
  a clear 911 block or warning (provider E911 needs a registered address); a
  settings card; testing against a real account.
- **Out of scope for a first version:** TLS/SRTP. The UA has neither, and TLS
  costs about 35 KB of heap on a board that's already tight; providers that
  require encryption (check Twilio/Telnyx) are out until then.
