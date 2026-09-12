# Future upgrade path: Bluetooth mic/speaker for a laptop or phone

Not implemented, not scheduled — a dev-journal note so this idea and the
reasoning behind it survive between sessions, per CLAUDE.md's convention for
open questions.

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
