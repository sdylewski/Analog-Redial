# Future upgrade path: Bluetooth mic/speaker for a laptop or phone

**Update (2026-09-12): now in progress, not just a note.** An ESP32-original
devkit is on its way to actually prototype the companion side. The C6 side
of the link now has real scaffolding: `firmware/components/bt_companion/`
(UART on GPIO16/17, a small line-based status/pairing protocol, an "enable
this daughter board" master switch) and a Settings -> Bluetooth tab, both
currently honest about there being no hardware to talk to yet. The rest of
this doc's reasoning stands unchanged -- it's the design record this
implementation is following. See bt_companion.h's own doc comment for the
protocol, and the pin table in the update below for the actual wiring.

Also worth noting for whenever a call-routing feature gets built on top of
this: FaceTime Audio/Video calls integrate into iOS's system telephony
(CallKit) the same as a normal cellular call, so a companion paired as a
standard Bluetooth HFP device gets the incoming-call `RING` indication and
answer/hangup control (`ATA`) for free, from the HFP spec itself -- no
custom iPhone app needed, the same way AirPods already alert you to and
answer FaceTime calls today. The only new firmware work is translating
that `RING` event into physically triggering this board's SLIC bell, and
the handset's hook-switch into sending `ATA` back -- both squarely within
the UART control link already being built for pairing/status.

Not implemented, not scheduled beyond that — the rest of this note is a
dev-journal record so this idea and the reasoning behind it survive between
sessions, per CLAUDE.md's convention for open questions.

## The idea

Use this board (or a companion module hanging off it) as a Bluetooth
mic+speaker for a laptop or cell phone — i.e. have it act like a Bluetooth
headset. The physical mic/speaker I/O already exists here (a real handset
through the ES8388 codec); the question is only whether Bluetooth Classic
audio can be added.

## Why the ESP32-C6 on this board can't do it directly

C6 has Bluetooth 5 (LE) only — no Bluetooth Classic (BR/EDR) radio. The two
profiles that make a device act like "a Bluetooth mic+speaker" to a laptop
or phone are both Classic-only:

- **A2DP** (music/audio streaming to a speaker)
- **HFP/HSP** (bidirectional call audio — mic *and* speaker)

BLE's native replacement, **LE Audio**, exists on paper, but host-side
support (macOS, Windows, most current laptop Bluetooth chipsets) is still
inconsistent enough that it wouldn't reliably "just pair" the way a real
Bluetooth headset does. Not a firmware gap — a silicon one.

## Options if this is ever pursued

1. **Swap the main board's MCU for an original ESP32** (Xtensa LX6
   WROOM/WROVER family) — has real Classic BT + BLE dual-mode, with working
   A2DP-sink and HFP examples in ESP-IDF's Bluedroid stack. This board's
   16MB flash requirement (recordings, prompts, dial-code audio, the
   bundled assets partition) isn't a blocker even here — Espressif sells
   "-N16" flash variants of the original WROOM/WROVER modules — but it
   means resourcing/re-pinning the whole board around a different chip
   family, not a drop-in change.
2. **Add a dedicated Bluetooth Classic audio module as a companion board**
   (preferred direction, given only 2-3 spare GPIO pins are available) —
   see below. Hands off audio to the existing codec in the analog domain,
   so it doesn't compete for digital I2S pins the way a second full ESP32
   would.

## Example module for option 2

**XS3868** (built on the CSR8635 Bluetooth audio SoC) — a well-documented,
hobbyist-friendly breakout board used in plenty of existing "add Bluetooth
to my speaker/car stereo" projects. Worth it here specifically because it
exposes *both* directions: stereo line-out (A2DP + HFP downlink) and a
differential mic input pair (HFP uplink) — not just a one-way "Bluetooth
receiver" board (many cheap AliExpress/Amazon "Bluetooth audio receiver"
boards are A2DP-only, no mic input, and wouldn't work for this). Control is
UART (AT commands) plus a couple of status/PIO pins for pairing state — fits
comfortably inside a 2-3 pin budget when only the analog audio lines and one
control line are actually needed.

More production-grade alternative: **BM62/BM64** (Microchip, formerly
BlueCreation) — the chip used inside a lot of real commercial Bluetooth
speakers/headsets, with official dev boards from Microchip. Better
documented from a single vendor and more robust, but less of a casual
$5-on-a-marketplace hobbyist buy than the XS3868.

Verify current availability/pricing/exact pinout before buying either —
not independently confirmed against a live listing here.

## Update (2026-09-12): BM64 looks like the frontrunner, pending real pin count

XS3868 turned out to be ~$50 on AliExpress and not stocked on Amazon at
all -- not the cheap hobbyist buy its reputation suggested. **BM64 modules
are on Digikey for ~$14**, which flips the earlier cost comparison: BM64 is
now the more accessible of the two, not the pricier one.

Two things learned about actually integrating it:

- **Physical**: BM64 comes as its own small module, not something that
  drops onto existing board space -- it needs either dedicated board area or
  a vertical/edge connector to mount it, i.e. a proper daughter-board
  arrangement rather than a few extra parts on the current PCB.
- **BM64 has an I2S interface**, so it can likely **tee into the existing
  I2S bus** (shared BCLK/WS with the main codec, per the "tap the existing
  bus" option above) rather than needing a fully separate digital audio
  path -- confirms that approach is viable for this specific chip, not just
  a theoretical option.
- **Revised pin count**: at least 3 control pins for the module itself, plus
  possibly the UART's 2 pins on top of that (exact requirement TBD -- some
  of BM64's control lines may be optional depending on which features are
  used). That's more than the "2-3 spare pins" this whole option was
  originally scoped around, once the I2S tap lines are added in -- **the
  real spare-GPIO count on the current board needs re-checking before
  this is a real plan**, not just a hopeful one.

Also worth recording: the original ESP32's pinout/footprint isn't
compatible with the ESP32-C6 dev kit this board is currently built around
either way -- so whether the eventual companion is a BM64 or (per option 1
above) a second ESP32, it's a **separate physical board wired in over a
pin header**, not something that mounts in place of anything already
there. If there's enough spare I/O to route out to a header, the plan is a
small daughter board carrying just the BM64 module, connected back to the
main board that way.

## Update (2026-09-12, continued): real pin count from the datasheet, and a correction

Pulled the actual BM64 datasheet (now in `datasheets/`) and the ESP32-C6's
own datasheet to check the pin math for real, instead of estimating.

**BM64 pin tables (Table 1-3) confirm:** I2S is 4 signals (`SCLK0`, `RFS0`,
`DR0`, `DT0`); UART is 2 (`HCI_TXD`, `HCI_RXD`). Everything else --
`RST_N`, `MFB`, `EAN`, `P2_0`, `LED1`/`LED2` -- can be hardwired instead of
consuming a GPIO: `RST_N` via a simple RC power-on-reset, `MFB` via a
physical pushbutton wired directly (bypassing the main board entirely) if a
manual pairing button is wanted, `EAN`/`P2_0` as fixed boot-mode straps for
Application mode (`EAN` already has an internal pulldown on flash-type
modules per the datasheet; `P2_0`'s required strap level needs checking
against the datasheet's Table 5-1), and `LED1`/`LED2` wired straight to
LEDs if a physical connection-status light is wanted, with no GPIO
round-trip through the main board.

**GPIO16/17 confirmed genuinely free**: defined in `board_pins_smd4.h`
(and `_smd.h`/`_tht.h`) as `BOARD_UART_TX`/`BOARD_UART_RX`, but never
referenced anywhere else in the firmware -- reserved for a debug UART that
was never actually wired up in software.

**Correction to the "just tee the existing I2S bus" idea above:** the
ESP32-C6's own datasheet (section 5.2.1.4) confirms it has only **one** I2S
controller, already fully occupied by the existing ES8388 codec's own
TX+RX. Sharing `BCLK`/`WS` across multiple slave listeners is fine (that
part was right), but the actual *data* lines can't simply be tee'd the way
described above -- a single I2S peripheral can't run two independent
full-duplex streams to two different chips at once without either TDM
slot-sharing (uncertain both chips support it) or the C6's CPU actively
bridging samples between two separate interfaces in firmware. Not a
passive hardware tap the way clock-sharing is.

**Better fix: use BM64's *analog* audio pins instead** (`AOHPL`/`AOHPR`/
`AOHPM` out, `MIC1_P`/`MIC1_N` in), wired directly into the existing
handset mic/earpiece analog paths. BM64's built-in DAC is spec'd at -98dB
SNR, so this isn't a quality compromise, and it sidesteps the I2S-sharing
problem entirely -- **zero C6 GPIO needed for audio.**

**Revised total**: with the analog-audio approach, the only new C6 GPIO
needed is the UART pair (`HCI_TXD`/`HCI_RXD`) -- which fits entirely on the
already-free GPIO16/17. The Line2/microSD 4-pin header isn't needed for
this feature at all this way -- it stays free, or there's slack to also
wire `RST_N`/`MFB` there for active control instead of hardwiring them.

## ESP32-original-as-companion, worked out for comparison

Prototyping plan (confirmed sound): validate on a DevKitC board already on
hand first, then move the *same* firmware (`idf.py set-target esp32`, no
code changes) to a bare ESP32-WROOM-32 module soldered onto a real
daughter board once the wiring/behavior checks out -- devkit and bare
module are the same chip.

Pin count comes out the same as the BM64 plan when also using an analog
handoff: 2 GPIO for UART coordination between the two chips (again,
GPIO16/17), 0 for audio. The real tradeoff is audio quality, not pin count:
the original ESP32's built-in DAC (GPIO25/26) is only 8-bit -- real audible
quantization noise, versus BM64's proper high-resolution DAC. ESP32-
companion wins on cost (~$5-8 vs. BM64's ~$14) and reuses a toolchain
already in use for this whole project; BM64 wins on audio quality.

**Pin table for the ESP32-original companion** (this is the one actually
being prototyped first):

| Signal | C6 (main board) | Companion (ESP32-original) | Notes |
|---|---|---|---|
| UART TX | GPIO16 | GPIO16 (RX2) | crossed -- C6's TX goes to companion's RX |
| UART RX | GPIO17 | GPIO17 (TX2) | crossed the other way |
| GND | GND | GND | common reference, required |
| 3.3V | 3V3 rail | 3V3 in | verify regulator headroom -- ~500mA BT TX peaks |
| Downlink audio (BT -> earpiece) | *(wired into the existing ES8388 analog path, not a C6 GPIO)* | GPIO25 (DAC1) | decoded call/music audio out |
| Uplink audio (handset mic -> BT) | *(tapped from the existing analog mic path, not a C6 GPIO)* | GPIO34 (ADC1_CH6) | feeds outgoing BT audio |

Companion-side UART pins chosen to match its own default UART2 pins
(GPIO16=RX2, GPIO17=TX2) purely as a mnemonic -- no electrical requirement
to use those specific numbers, the GPIO matrix can route UART to any pins
on either chip.

### WROOM-32 module variants, for whenever this is actually bought

- **ESP32-WROOM-32** (no suffix) -- the original module, oldest silicon
  revision. Espressif has been phasing this one out; still floating around
  in older stock, increasingly hard to find as new.
- **ESP32-WROOM-32D** -- a respin (ESP32-D0WD-V3 die) with manufacturing/
  errata improvements, and the generation that introduced clear flash-size
  SKUs (-N4/-N8/-N16 for 4/8/16MB).
- **ESP32-WROOM-32E** -- the current/recommended module as of this
  writing, same D0WD-V3 die as -32D plus an updated antenna/RF matching
  design. What most new stock actually is today.

None of this matters functionally for this project (all three have the
same real Classic BT + BLE dual-mode radio) -- buy whichever's actually in
stock, preferring -32E if given the choice since it's current production.
Flash size doesn't need to be large for a Bluetooth-bridge-only companion
(unlike this project's own 16MB main board) -- the base 4MB (-N4) SKU is
plenty.

## Known tradeoffs, independent of which module

- **Audio quality ceiling is the Bluetooth profile spec, not chip speed**:
  A2DP uses SBC (compressed, fine for casual listening, not audiophile);
  HFP call audio is narrowband CVSD (8kHz) unless both ends negotiate
  wideband mSBC (16kHz). Every commercial Bluetooth headset lives with the
  same ceiling — no amount of extra MHz on either side changes it.
- HFP is the less mature of the two profiles in ESP-IDF's own Bluedroid
  stack specifically — another reason a dedicated audio module (which
  implements HFP in its own firmware, independent of ESP-IDF) is the safer
  bet over a second ESP32 running Bluedroid for this particular feature.
