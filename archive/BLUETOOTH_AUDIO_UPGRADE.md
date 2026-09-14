# Bluetooth mic/speaker for a laptop or phone

In progress: an ESP32-original **DevKitC-style board** (not a bare module)
is on its way to be the companion, permanently -- decided 2026-09-13, see
"Decision" below for why. The C6 side already has real scaffolding --
`firmware/components/bt_companion/` (UART on GPIO16/17, a small line-based
status/pairing protocol, an "enable this daughter board" master switch) and
a Settings -> Bluetooth tab, both currently honest about there being no
hardware to talk to yet. See `bt_companion.h`'s own doc comment for the
protocol.

## The idea

Use this board (or a companion module hanging off it) as a Bluetooth
mic+speaker for a laptop or cell phone -- i.e. have it act like a Bluetooth
headset. The physical mic/speaker I/O already exists here (a real handset
through the ES8388 codec); the question is only whether Bluetooth Classic
audio can be added.

## Why the ESP32-C6 on this board can't do it directly

C6 has Bluetooth 5 (LE) only -- no Bluetooth Classic (BR/EDR) radio. The two
profiles that make a device act like "a Bluetooth mic+speaker" to a laptop
or phone are both Classic-only: **A2DP** (music/audio streaming) and
**HFP/HSP** (bidirectional call audio -- mic *and* speaker). BLE's native
replacement, LE Audio, exists on paper, but host-side support (macOS,
Windows, most current laptop Bluetooth chipsets) is still inconsistent
enough that it wouldn't reliably "just pair" the way a real Bluetooth
headset does. Not a firmware gap -- a silicon one. This applies to every
current Espressif chip except the original ESP32 (Xtensa LX6) -- C2, C3,
C6, H2, S3 are all LE-only or BLE-less, regardless of "Bluetooth 5.0"
marketing (that number describes the LE spec revision, not Classic
support).

Either option below needs a separate physical daughter board wired in over
a pin header -- the original ESP32's pinout/footprint isn't compatible with
the ESP32-C6 dev kit this board is built around, so there's no drop-in
swap either way.

Audio hands off in the **analog domain** in both options, not digitally:
the C6 has only one I2S controller (confirmed via its own datasheet,
section 5.2.1.4), already fully occupied by the existing ES8388 codec, so
a second digital audio stream would need either TDM slot-sharing (uncertain
support on either chip) or CPU-mediated sample bridging in firmware.
Analog sidesteps that entirely and costs zero extra C6 GPIO for audio --
the companion's own audio pins wire directly into the existing handset
mic/earpiece paths.

## Option A: BM64 module (Microchip, formerly BlueCreation)

~$14 on Digikey. The chip used inside a lot of real commercial Bluetooth
speakers/headsets -- well-documented from a single vendor, more robust than
a hobbyist breakout, but comes as its own small SMD module needing a
vertical/edge connector or dedicated daughter-board space, not something
that drops onto existing board area.

Pin plan (from the datasheet's Table 1-3, now in `datasheets/`):

| Signal | C6 | BM64 | Notes |
|---|---|---|---|
| UART TX | GPIO16 | `HCI_RXD` | crossed by *role*, not by wiring two different pin numbers together -- see Option B's note on this below, same mistake risk applies here |
| UART RX | GPIO17 | `HCI_TXD` | crossed by *role* |
| GND | GND | `GND` | |
| 3.3V | 3V3 rail | `BAT_IN` | accepts 3.2-4.2V directly |
| Downlink audio | *(existing ES8388 analog path)* | `AOHPL`/`AOHPR`/`AOHPM` | BM64's own DAC, -98dB SNR -- no quality compromise |
| Uplink audio | *(existing handset mic path)* | `MIC1_P`/`MIC1_N` | |

Everything else on the module (`RST_N`, `MFB`, `EAN`, `P2_0`, `LED1`/`LED2`)
can be hardwired instead of consuming a GPIO: `RST_N` via a simple RC
power-on-reset, `MFB` via a physical pushbutton if a manual pairing button
is wanted, `EAN`/`P2_0` as fixed boot-mode straps for Application mode
(`EAN` already has an internal pulldown on flash-type modules per the
datasheet; confirm `P2_0`'s required level against the datasheet's Table
5-1), `LED1`/`LED2` wired straight to LEDs for a physical status light with
no C6 GPIO round-trip. **Net new C6 GPIO: just the UART pair**, which fits
entirely on GPIO16/17 (confirmed genuinely free -- defined in every board's
pin map as a debug UART, never actually used in firmware).

## Option B: ESP32-original DevKitC board as a companion (the current plan)

The chip itself has real Classic BT + BLE dual-mode, with working A2DP-sink
and HFP examples in ESP-IDF's own Bluedroid stack -- same toolchain already
used for this whole project. Cheaper than BM64, at the cost of audio
quality: the original ESP32's built-in DAC (GPIO25/26) is only 8-bit, real
audible quantization noise versus BM64's proper high-resolution DAC.

**Using the full DevKitC board, not a bare WROOM-32 module -- decided
2026-09-13, permanently, not just for prototyping.** A bare module is
cheaper and smaller, but needs its own 3.3V regulator, USB-to-serial
adapter for programming, and status LEDs added on the daughter board; the
devkit already has all three built in, and the extra board space is a
non-issue here. Traded a little board area for a lot of fungibility --
any DevKitC-style board works, easy to swap/debug/reprogram over USB like
every other board in this project, no separate programmer needed.

**Regulator note** (came up while sourcing this): the ESP32 module itself
needs regulated 3.3V, as its own datasheet says -- but the *devkit board*
has its own onboard LDO regulator between its 5V/VIN pin and the module's
actual 3.3V supply, which is exactly why devkits expose both a 5V/VIN pin
(regulator input) and a separate 3.3V pin (regulator output). Powering the
devkit from a 5V rail into VIN (or straight off USB, same thing) is the
normal, intended way to run it -- not a workaround.

Pin plan:

| Signal | C6 side | Companion (ESP32-original DevKitC) | Notes |
|---|---|---|---|
| UART TX | GPIO16 | GPIO16 (RX2) | same pin *number* both ends -- see note below, this isn't a typo |
| UART RX | GPIO17 | GPIO17 (TX2) | same pin *number* both ends |
| GND | GND | GND | shared ground, required for the UART link to work at all -- confirmed 2026-09-14 this is easy to skip by accident on a breadboard |
| 5V | 5V rail (or USB) | 5V/VIN pin | devkit's onboard regulator drops this to the module's 3.3V |
| Downlink audio | AG1171 **Vin** pin, past its own 10nF coupling cap | GPIO25 (DAC1), **through its own new 100nF series cap** | see "Audio tap components" below |
| Uplink audio | AG1171 **Vout** pin, past its own 100nF coupling cap | GPIO34 (ADC1_CH6), **plus a new bias divider** | see "Audio tap components" below |
| Pairing-status LED | *(none -- companion-only)* | GPIO2 (onboard LED on most genuine DevKitC boards) | blinks while pairing, solid once connected, off otherwise -- see `bt_led.c` on the companion firmware |

**On "same pin number, crossed role"**: C6's GPIO16 is *its* UART TX; the
companion's GPIO16 is labeled RX2 on *its* side. Wire pin 16 to pin 16 and
pin 17 to pin 17 straight across (not diagonally to the other number) --
confirmed 2026-09-14 this is exactly the mistake that caused three failed
pairing/flashing sessions: wiring by "TX goes to RX" instinct sent one leg
to the companion's GPIO1/GPIO3 (its real USB-serial bootloader pins)
instead, which also explained why flashing the companion over USB kept
failing whenever that leg was connected.

Companion-side UART pins match its own default UART2 pins purely as a
mnemonic -- no electrical requirement, the GPIO matrix can route UART to any
pins on either chip.

### Audio tap components (new 2026-09-14, bench-bring-up wiring)

**Never tap a raw AG1171 pin directly** -- Vin/Vout sit in the SLIC's own
5V-referenced domain. Both taps below go *after* the SLIC's own existing
coupling cap, at the same already-AC-only node the ES8388 codec itself
uses (see `docs/signal-levels.md` §3: Vin has a 10nF series cap toward the
codec's DAC output, Vout has a 100nF series cap toward the codec's ADC
input) -- these are the exact same physical pins/traces the codec already
uses, not new dedicated pins.

- **GPIO25 -> Vin (downlink): needs its own new 100nF series coupling
  cap**, in-line between the GPIO25 pin and the Vin tap point -- not
  optional. The ESP32's DAC output is DC-biased (an 8-bit unsigned DAC
  centered around ~1.65V, not 0V), and Vin past the SLIC's own cap is
  meant to be a clean AC-only node; wiring GPIO25 there with no cap of its
  own would dump that DC bias straight onto a node the hybrid's own
  balance depends on. A DAC *output* actively drives the node, unlike an
  ADC *input* below, which only listens -- that asymmetry is why only this
  side needs a cap of its own.
- **GPIO34 -> Vout (uplink): no new series cap needed** (it's already
  downstream of the SLIC's own 100nF cap, a clean AC node an ADC can share
  directly) **but needs a new bias divider** -- two resistors, ~100kΩ from
  GPIO34 to the 3.3V rail and ~100kΩ from GPIO34 to GND, meeting at the
  GPIO34/Vout junction. This centers the AC signal at ~1.65V, the DC
  reference the ESP32's own ADC needs to read a signal that otherwise has
  no DC reference at all (an ADC *input* doesn't drive current back onto
  the node the way a DAC output does, so it doesn't need its own blocking
  cap -- it just needs *something* to establish a bias point). High
  resistance on both legs keeps this from loading the existing AC signal
  or the codec's own 20kΩ ADC input impedance.

Component values above are reasonable starting points, not yet bench-
verified against a scope -- confirm with an oscilloscope on GPIO25/GPIO34
before treating them as final (measuring actual DC bias and AC swing at
each pin, per the existing DTMF-headroom divider worked out the same way
in `docs/signal-levels.md` §3).

## Decision: ESP32 DevKitC first, BM64 as the fallback

Going with Option B (ESP32-original DevKitC) as the actual build -- cheaper,
reuses this project's own toolchain, and a devkit is already in hand to
start with. **If its audio quality (8-bit DAC) turns out not good enough
in practice**, BM64 (Option A) is the documented fallback -- pin plan
above is ready to go, just needs sourcing a module (Digikey, ~$14, Class 2
per an actual commercial part number seen: `BM64SPKS1MC2-0003AA` or
similar -- confirm the exact SKU/firmware variant against Microchip's
ordering guide before buying, the datasheet alone doesn't spell out what
the trailing suffix variants mean).

## Known tradeoffs, independent of which option

- **Audio quality ceiling is the Bluetooth profile spec, not chip speed**:
  A2DP uses SBC (compressed, fine for casual listening, not audiophile);
  HFP call audio is narrowband CVSD (8kHz) unless both ends negotiate
  wideband mSBC (16kHz). Every commercial Bluetooth headset lives with the
  same ceiling -- no amount of extra MHz on either side changes it.
- HFP is the less mature of the two profiles in ESP-IDF's own Bluedroid
  stack specifically -- one more reason BM64 (which implements HFP in its
  own firmware, independent of ESP-IDF) may end up the safer long-term bet
  even though ESP32-original is what's being tried first.

## FaceTime / incoming-call ringing, for whenever call routing gets built

FaceTime Audio/Video calls integrate into iOS's system telephony (CallKit)
the same as a normal cellular call, so a companion paired as a standard
Bluetooth HFP device gets the incoming-call `RING` indication and
answer/hangup control (`ATA`) for free, from the HFP spec itself -- no
custom iPhone app needed, the same way AirPods already alert you to and
answer FaceTime calls today. The only new firmware work is translating that
`RING` event into physically triggering this board's SLIC bell, and the
handset's hook-switch into sending `ATA` back -- both squarely within the
UART control link already being built for pairing/status, not new scope.
