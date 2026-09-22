# Bluetooth mic/speaker for a laptop or phone

**Switched to BM64 (Option A) -- decided 2026-09-15, see "Confirmed: ESP32
software HFP audio is a dead end" below.** The ESP32-original DevKitC
companion (Option B) was the initial build and is kept below as a real,
working record of that bring-up -- its UART/pairing/analog-bridge design is
still directly reused for BM64, just swapping which chip sits on the other
end of the same UART link and analog taps. The C6 side already has real
scaffolding -- `firmware/components/bt_companion/` (UART on GPIO16/17, a
small line-based status/pairing protocol, an "enable this daughter board"
master switch) and a Settings -> Bluetooth tab. See `bt_companion.h`'s own
doc comment for the protocol.

## Confirmed 2026-09-15: ESP32 software HFP audio is a dead end

Bench-tested end to end on real hardware (WWV call over a live HFP
connection): DAC output confirmed correct (clock-source bug fixed, scope-
verified waveform), the callback-blocking bug fixed (decoupled downlink
writes from Bluedroid's own thread), a downlink AGC tried and reverted (made
it worse, not better) -- and voice audio still came out "garbled, like
underwater" while simple tones stayed clean.

Built a diagnostic (a `capture_downlink` console command in the
now-removed `bt_companion_hfp_test` project, plus `tools/capture_downlink.py`
on the host side) that captured the raw PCM samples
Bluedroid hands the app -- before ANY of our own code touches them -- and
analyzed the spectrum. Result: **79% of all energy sat below 300Hz, ~0%
above 2000Hz**, on a live voice call. That is not what decoded speech looks
like at any quality level; normal voice carries most of its intelligibility
in roughly 300-3400Hz (formants). The raw capture and a simulation of our
own 16-to-8-bit DAC conversion were nearly identical, which rules out
everything downstream of Bluedroid: the DAC, the conversion, the AGC, the
analog bridge, all confirmed clean.

Traced into ESP-IDF's own Bluedroid source
(`components/bt/host/bluedroid/btc/profile/std/hf_client/bta_hf_client_co.c`):
for CVSD (this project's negotiated codec -- WBS/mSBC explicitly disabled),
the host-side C code does a **byte-for-byte passthrough, zero decode**.
Compare mSBC, which explicitly runs a real software decoder in the same
file. CVSD decode is expected to happen entirely inside the ESP32 Bluetooth
controller's own closed-source firmware -- confirmed by ESP-IDF's own
`hfp_hf` example README: *"hardware is responsible for the codec job and
CVSD is in use."* Nothing in the open-source host stack can be inspected or
patched to fix this.

This matches a well-documented, years-old, unresolved community pattern on
this exact code path (`CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI`/vHCI + CVSD),
not something specific to this project:
- [espressif/esp-idf#6996](https://github.com/espressif/esp-idf/issues/6996) -- "Audio samples droped in HFP (CVSD codec, VHCI)", recorded waveform loses 60 samples every few seconds. Marked "Done" internally by Espressif; no public fix or explanation ever posted.
- [espressif/esp-idf#4824](https://github.com/espressif/esp-idf/issues/4824) -- a user fixed a real config-macro typo in this same `hfp_hf` example and still got "low quality voice", `SCO xmit Q overflow, pkt dropped`, and device resets after a few seconds. Still open, no official fix.
- [espressif/esp-idf#7222](https://github.com/espressif/esp-idf/issues/7222) -- "regular audible distortion... popping and glitching", reported across multiple ESP-IDF versions (4.3.2, 4.4.1).

**Conclusion**: this is a genuine limitation of the ESP32's classic
Bluetooth controller firmware for software (vHCI) HFP audio, not a bug in
our code, and not something fixable from the application side. BM64 avoids
it entirely by doing HFP audio decode in its own dedicated, mature firmware
-- independent of ESP-IDF's Bluedroid stack altogether.

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

## Option A: BM64 module (Microchip, formerly BlueCreation) -- current plan

~$14 on Digikey. The chip used inside a lot of real commercial Bluetooth
speakers/headsets -- well-documented from a single vendor, more robust than
a hobbyist breakout, but comes as its own small SMD module needing a
vertical/edge connector or dedicated daughter-board space, not something
that drops onto existing board area. Datasheet:
`datasheets/BM62-64-Bluetooth-Stereo-Audio-Module-DS60001403.pdf` (covers
BM62 and BM64 together -- BM64 is the 43-pin variant, BM62 is a cheaper
37-pin subset without I2S/USB; the pin table below is BM64's, per its own
Table 1-3/Figure 1-5).

Pin plan, real pin names/numbers confirmed against the datasheet
2026-09-15:

| Signal | C6 side | BM64 pin (name / no.) | Notes |
|---|---|---|---|
| UART TX | GPIO16 | `HCI_RXD` (pin 19) | crossed by *role* (BM64's own RXD, not a pin-number swap) |
| UART RX | GPIO17 | `HCI_TXD` (pin 20) | crossed by *role* |
| GND | GND | `GND` (pins 14, 43) | |
| 3.3V-4.2V | 3V3 rail (or a dedicated regulated rail) | `BAT_IN` (pin 22) | Not actually a battery-only pin despite the name -- datasheet: "Source can either be a Li-Ion battery or any other power rail on the host board." Under 2.9V, the module won't boot at all (built-in UVP) -- confirm the C6's own 3.3V rail sags nowhere near that under load. |
| Downlink audio | AG1171 **Vin**, past its own 10nF cap | `AOHPL` (pin 7), through a new **47 uF** DC-blocking cap | Single-ended mode per the datasheet's own Figure 2-8 ("driving an external audio amplifier" -- our SLIC's Vin *is* that external amplifier's input as far as BM64 is concerned) -- mono, so only one of AOHPL/AOHPR is needed, `AOHPM` (pin 6, "capless mode" common-mode pin) stays unconnected in single-ended mode. **47 uF confirmed 2026-09-16**: not a derived estimate, it's the exact value the datasheet itself used to characterize the DAC's single-ended-mode frequency response (Figure 2-6's own caption). **Footprint (researched 2026-09-16): use 10 uF instead, not 47 uF.** 47 uF isn't the electrically-required value here -- it's just what Microchip's test jig happened to use -- and there's no genuine 47 uF part in JLCPCB's cheaper "Basic" library at any footprint (0402/0603/0805/1206): every "47uF Basic" part in a widely-circulated community parts list turned out to be mislabeled 4.7 uF on closer check against LCSC's own listings, and the one real 47 uF part found (C136751, tantalum, EIA 3528/Case B) is "Extended" category, which costs extra per unique part per JLCPCB's assembly pricing. A 10 uF, 0805, Basic ceramic (**C15850**, Samsung CL21A106KAYNNNE) is both real-Basic and electrically overkill for this job: even against a worst-case few-hundred-ohm SLIC Vin impedance, 10 uF puts the high-pass corner well under 10 Hz, far below the 300-3400 Hz voice band -- 47 uF's only advantage was matching the datasheet's own test setup, not anything this circuit needs. |
| Uplink audio | AG1171 **Vout**, past its own 100nF cap | `MIC_P1` (pin 9), through a new **1 uF** DC-blocking cap | `MIC_N1` (pin 8) -- differential mic input; our source is single-ended (the SLIC's own AC-only Vout tap), so tie `MIC_N1` to a bias reference (ground, or per a real electret-mic reference circuit) rather than driving it -- **confirm the exact tie-off against the datasheet's own microphone application circuit before building**, not yet found in the pages read so far. `MIC_BIAS` (pin 10) only matters if a real electret capsule is attached directly; not needed for a line-level signal from the SLIC. **1 uF confirmed 2026-09-16**: calculated, not datasheet-given -- MIC input impedance is 24 kOhm typical (Electrical Characteristics table), and 1 uF against 24 kOhm puts the high-pass corner around 6.6 Hz, comfortably below the 300-3400 Hz voice band. 0.22-0.47 uF would also work for voice; 1 uF is a standard, cheap value with margin to spare. A 1 uF ceramic in 0603 (or 0402 if space is tight) is a genuine JLCPCB Basic part at this voltage rating -- no footprint concern here. |
| Reset | *(none -- no C6 connection)* | `RST_N` (pin 13) | Active-low. Datasheet, verbatim: "The RST_N signal input is active-low and no connection is required in most applications." Confirmed 2026-09-16 -- no RC circuit, no C6 GPIO. |
| Mode select | *(none -- no external component needed, see below)* | `EAN` (pin 32) + `P2_0` (pin 41) | **Re-confirmed 2026-09-16** -- see the correction just below the table (this row's own history has two corrections layered on it; the *second* one is the one to trust). |
| Pairing button (optional) | *(none)* | `MFB` (pin 26) | Multi-Function Button / power-on key -- wire to a pushbutton if a manual pairing trigger is wanted, same role `bt_led.c`'s pairing state serves on the ESP32 companion today. |
| Status LEDs (optional) | *(none)* | `LED1` (pin 28), `LED2` (pin 27) | Direct LED drivers, no GPIO round-trip through the C6 needed. |

**EAN/P2_0 -- two corrections layered on this row, history kept so the
reasoning isn't lost:**

1. *Original plan:* guessed "EAN's internal 4.7kOhm pulldown, so
   Application mode is likely the natural power-up default" -- flagged as
   unconfirmed.
2. *First correction (2026-09-16, wrong):* Table 5-1 (System
   Configuration I/O Pin Settings, in the generic Application Information
   section)'s own Note 1 says *"The P2_0, P2_4 and EAN pins have internal
   pull **high**"* -- read as contradicting #1, concluded EAN needs an
   external pull-down.
3. *Second correction (2026-09-16, the one to trust):* Table 1-3 -- the
   **module** pin description (not the generic chip-level table) --
   describes pin 32 (EAN) directly: *"Flash type module already installed
   internal pull down resistor 4.7 kOhm to GND."* Checked the Ordering
   Information table (Section 10.0): every BM62/64 SKU Microchip actually
   sells is described the same way -- there's no separate "ROM type"
   purchasable option. So Table 5-1's "pull high" note is almost
   certainly describing the bare IC's default before Microchip's own
   module-level modification, while Table 1-3 describes what's actually
   on the assembled module (what gets bought and soldered down) -- a real
   4.7kOhm pull-down already built in. **#1 was right all along; #2 was
   the wrong correction.**

Table 5-1 (System Configuration I/O Pin Settings) for reference:

| P2_0 | EAN | Mode |
|---|---|---|
| High | Low  | **Flash Application** (normal runtime -- the mode we want, and the module's own built-in pull-down already lands here with EAN left floating) |
| High | High | ROM Application |
| Low  | Low  | Flash IBDK |
| Low  | High | **ROM TEST** (this is the DFU/firmware-programming mode) |

So: **leave both EAN and P2_0 floating -- no external pull resistor on
either pin.** The module's own built-in pull-down on EAN plus its
internal pull-up on P2_0 already select Flash Application mode with zero
external components.

**Does the module need firmware loaded, and can it be done over UART?**
Yes to both, but it's a real design fork, not just a wiring detail:
- BM64 ships from Microchip pre-programmed (HFP/A2DP/AVRCP) -- nothing
  has to be loaded to get a working module.
- DFU (updating that firmware later) is possible over the *same* UART
  pair already wired for HCI, per 5.2.1.1 "Enter BT Module/MCU DFU mode":
  *"To upgrade the flash through UART, first select the module in 'ROM
  TEST' mode"* (P2_0 low, EAN high -- see Figure 5-9's timing diagram),
  do the write, then switch back to Application mode (Figure 5-10)
  afterward. The datasheet doesn't spell out the actual UART DFU packet
  format here -- that lives in Microchip's PC-side "Flash tool," not in
  this doc -- so exercising this would need more reverse-engineering or
  an app note if it's ever wanted.

**Decision: leave EAN and P2_0 unconnected (no pull resistors, no C6
GPIO).** Zero external components, fixed permanently in Flash Application
mode by the module's own built-in strapping, matches "net new GPIO: just
the UART pair" below. This forecloses in-circuit reflashing from the C6
-- if that turns out to matter later (a Microchip firmware update, custom
EQ/codec config), EAN and P2_0 would need to move to spare C6 GPIOs
instead, so firmware could actively drive them into ROM TEST mode on
demand (overpowering the module's own weak internal pulls is well within
a normal GPIO output's drive strength). Revisit only if that need
actually comes up; the stock firmware is almost certainly sufficient for
this board's HFP-only use case.

**Net new C6 GPIO: just the UART pair**, same as the ESP32 companion below
-- GPIO16/17 (confirmed genuinely free -- defined in every board's pin map
as a debug UART, never actually used in firmware).

Everything else on the module not listed above (`P0_x`/`P1_x`/`P2_x`/`P3_x`
GPIO, `AIL`/`AIR` line-in, `DP`/`DM` USB, `AMB_DET` thermal sense,
`ADAP_IN`/`SYS_PWR` battery-charging pins) isn't needed for this design --
BM64's built-in Li-ion charger/thermal-protection features are irrelevant
since it's powered from the C6's own regulated rail, not a battery.

## Option B: ESP32-original DevKitC board as a companion (superseded 2026-09-15, code removed 2026-09-21)

**No longer the plan** -- see "Confirmed: ESP32 software HFP audio is a dead
end" above for why. The chip itself has real Classic BT + BLE dual-mode,
with working A2DP-sink and HFP examples in ESP-IDF's own Bluedroid stack --
cheaper than BM64, at the cost of audio quality (the original ESP32's
built-in DAC is only 8-bit) and, as it turned out, a hard CVSD-decode
limitation in the Bluetooth controller firmware itself that no amount of
application-side fixing could work around.

The two standalone reference/bring-up projects this used
(`bt_companion_fw/`, an A2DP-sink example; `bt_companion_hfp_test/`, the
HFP bring-up + `capture_downlink` diagnostic that produced the dead-end
finding above) were removed from the repo once BM64 became the real plan --
their UART link/pairing-protocol/analog-bridge-wiring *approach* carried
over to BM64's own bring-up, but the actual ESP32-Bluedroid code itself had
no further use. See git history before 2026-09-21 to recover them if ever
needed again.

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

## Decision: BM64 (updated 2026-09-15, was ESP32 DevKitC first)

Tried Option B (ESP32-original DevKitC) first -- cheaper, reused this
project's own toolchain, a devkit was already in hand. Confirmed on real
bench hardware that its audio path is a dead end (see "Confirmed" section
above) -- not a tuning problem, a genuine closed-source controller-firmware
limitation. Switching to BM64 (Option A): pin plan above is ready to go,
just needs sourcing a module (Digikey, ~$14, Class 2 per an actual
commercial part number seen: `BM64SPKS1MC2-0003AA` or similar -- confirm
the exact SKU/firmware variant against Microchip's ordering guide before
buying, the datasheet alone doesn't spell out what the trailing suffix
variants mean).

## Known tradeoffs, independent of which option

- **Audio quality ceiling is the Bluetooth profile spec, not chip speed**:
  A2DP uses SBC (compressed, fine for casual listening, not audiophile);
  HFP call audio is narrowband CVSD (8kHz) unless both ends negotiate
  wideband mSBC (16kHz). Every commercial Bluetooth headset lives with the
  same ceiling -- no amount of extra MHz on either side changes it.
- HFP being the less mature of the two profiles in ESP-IDF's own Bluedroid
  stack turned out not to be a theoretical concern -- see "Confirmed:
  ESP32 software HFP audio is a dead end" above for what actually broke
  and why BM64 (implementing HFP in its own firmware, independent of
  ESP-IDF) avoids that specific failure mode entirely.

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
