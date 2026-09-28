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

## Confirmed 2026-09-22: BM64 UART protocol + pairing work end-to-end on real hardware

`firmware/components/bt_companion/` fully rewritten to speak BM64's real
binary UART protocol (see its own doc comment for the frame format) and
bench-confirmed against a real BM64 module: command/ACK/event framing and
checksums all correct (`GET_UART_VERSION`, `READ_LOCAL_BD_ADDR` both
verified), and **pairing confirmed working end-to-end -- a real iPhone
found the module (advertised as the board's own name, e.g. "dualPhone")
and paired**.

The pairing mystery took a while to run down: sending the pairing MMI
action (`0x5D`) alone got ACKed (status 0x00, i.e. accepted) but the module
never actually broadcast as discoverable -- no state-change event followed
at all, and no phone/laptop ever saw it, even across a full power cycle and
several clean re-tries. The fix, found in `reference/BM64_arduino`'s own
`powerOn()`: the module needs an MMI `POWERON_BUTTON_PRESS` (`0x51`) then
`POWERON_BUTTON_RELEASE` (`0x52`) sent first -- only after that handshake
did the module start emitting real `BTM_Status` events (Power ON -> Standby
-> Pairing/discoverable), confirmed against Microchip's actual "Audio UART
Command Set" doc (now in `datasheets/`, not just the third-party library).
`bt_companion_start_pairing()` and the poll task's own startup sequence
both send this handshake now.

Also confirmed: MFB does not need to be released back low after its
initial power-on assertion for any of this to work (an earlier hypothesis,
disproven) -- the bench wiring (a fixed pull-up to VCC, no GPIO control)
works fine held high indefinitely. The actual missing piece was purely the
UART-side button handshake above.

Not yet tested: actual audio quality over the paired link (dial a real
number -- WWV's time signal is the established test tone for this, see the
2026-09-15 HFP bench test below -- and record off the line to listen back).
`BM64_EVENT_DEVICE_STATE`'s state table is now fully known from the real
doc (not guessed), but the exact conditions under which the module
auto-reconnects to an already-bonded phone after an ESP32-side reboot
aren't confirmed yet -- observed once needing a manual reconnect from the
phone's Bluetooth settings after a fresh flash.

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

### Confirmed 2026-09-26: the AOHPL tap measurably loads and distorts Vin

Measured directly at the SLIC VIN pin (same point used to debug the
spkamp/LOUT1VOL bug above), `spkamp 32`, 1 kHz test tone: **1.97 Vpp with
the BM64's AOHPL wire disconnected from Vin, dropping to 1.1 Vpp -- and
visibly more square than sinusoidal -- with it reconnected.** That's
roughly a 5 dB loss plus real harmonic distortion, not just a benign
AC-coupled "branch" as the pin-plan table above assumed. The two coupling
caps (SLIC's own 10 nF toward the codec, BM64's new 10 uF toward AOHPL) do
correctly block DC between the two branches, but a series cap says nothing
about the *AC* loading of whatever sits behind it -- and AOHPL's own output
impedance (or the offset/bias network Figure 2-8's single-ended mode
uses internally) is evidently low enough, and/or nonlinear enough near the
signal's peaks, to noticeably sag and clip a signal designed only around
the codec's own ~10s-of-kOhm expectations on that node.

Not yet root-caused to a specific mechanism (need the BM64 datasheet's
AOHPL output-impedance/single-ended-mode section, and ideally a scope
capture of AOHPL's own waveform in isolation to see whether the distortion
originates at the BM64 side or only appears once it's fighting the shared
node) -- but it's real, reproducible, and worth designing around before
finalizing SMD5's BM64 header: either a unity-gain buffer between AOHPL and
the shared Vin node (isolates the two branches' AC loading from each other,
the textbook fix), or accept the loss and compensate with a few dB of extra
`spkamp` gain when BM64 is the active path -- but the *distortion* (not
just the level drop) argues for the buffer, since gain can't undo clipping
that already happened upstream of it.

### Adding a BM64 to a 2-line SMD4 board (decided 2026-09-27)

A 2-line SMD4 has line 1's SLIC wired to GPIO18-21 through J5's four
shunts. The BM64's MFB/RST_N/P2_0 need those same three GPIOs (18-20), so
the two can't share them. The plan: **pull J5's four shunts** -- line 1's
SLIC stays stuffed but is electrically disconnected -- and wire the BM64's
three control pins to J5's GPIO-side pins instead. Then in Settings ->
Hardware set **"Neither (one phone line, no microSD)" + "BM64 module
fitted"**. That's a normal one-line + Bluetooth board as far as firmware is
concerned, so both normal operation and EEPROM programming
(`bt_companion_ibdk_enter()`, which refuses on a 2-line config) work
without further changes. Restoring line 1 later is just re-fitting the
shunts and switching back to two lines.

SMD5 plans to avoid this trade by giving MFB its own GPIO and using J5 only
temporarily for programming -- see
[`hardware/ANALOG_REDIAL_SMD5.md`](hardware/ANALOG_REDIAL_SMD5.md).

### TODO 2026-09-27: protect Vin from the BM64 before wiring another SLIC

A SLIC died during this bench work (newphone, line 0): its DC feed and hook
detection still work perfectly (tip 51 V on-hook -> 14 V off-hook, SHK
swings 0/3.2 V) but its **2-wire/4-wire audio hybrid is dead** -- a clean
dial tone at Vin produces no AC on tip at all. Whether that was caused by
the BM64 hookup or was a bad clone from the start is unresolved (check
dualphone/SMD2 line 0, which also had BM64 on it, for a second data point).

**The key lesson regardless: a series coupling cap is not protection.** It
blocks steady DC and nothing else. Ranked suspects, all present in this
bench setup:

1. **Hot-plug charge dump.** That wire was connected/disconnected many
   times with both sides powered. A cap bridging two nodes at different
   potentials is a momentary short -- 10 uF across a few volts is a current
   impulse limited only by wire resistance.
2. **Ground integrity.** If the module's ground return ever opened (or was
   marginal) while the signal wire stayed connected, Vin becomes the return
   path and can see the full supply differential. This rig is jumper wires,
   and a real grounding fault did show up on the scope side the same day.
3. **Transients straight through the cap.** Fast edges pass essentially
   unattenuated -- module power-on/off pops, DAC enable/disable steps. A
   3-5 V fast edge at AOHPL lands at Vin nearly intact.
4. **Tip/ring bridging back during probing.** Tip sits at ~51 V; a probe
   slip to the Vin node is far outside what a low-level audio input on that
   chip tolerates.

**Vin input impedance is 60 kOhm** (AG1171 datasheet, Electrical
Characteristics: "Input Impedance at VIN 60 kohm"; VOUT output impedance is
10 Ohm). That's the number the earlier `BM64_AUDIO_DEBUG.md` guess of "a
few hundred ohms worst case" was standing in for, and it makes protection
cheap:

- **Series resistor is nearly free.** 1 kOhm against 60 kOhm is ~0.14 dB of
  loss; even 4.7 kOhm costs under 1 dB. Use it to limit inrush to a few mA.
- **Clamp diodes alone are not enough** -- they bound the voltage but do
  nothing about the cap's inrush current. Series R + clamp diodes (or a
  low-capacitance TVS) to the SLIC's local rails, together, is the right
  combination. Put both at the Vin tap.
- **Also worth fixing while in there:** the existing 10 nF cap between the
  codec's LOUT1/ROUT1 and Vin sets a high-pass corner at 1/(2*pi*60k*10n)
  = **265 Hz**, which is *inside* the 300-3400 Hz voice band -- the bottom
  of the band is rolling off today. 100 nF would put the corner near 26 Hz.
  Unrelated to the dead SLIC, but a real find from the same datasheet
  number.

**Proposed Vin interface (schematic).** Three sources meet at the SLIC's Vin
pin -- the codec's DAC, the BM64's analog output, and (unavoidably) whatever
noise the wiring picks up. Each source gets its own series coupling cap so
neither one's DC bias reaches the other, and the BM64 branch additionally
gets the series resistor + clamp diodes that this SLIC died for the lack of:

```text
  ES8388 (codec)                                     AG1171 SLIC (U4/U3)
  ┌──────────────┐                                   ┌──────────────────┐
  │              │        C_codec                    │                  │
  │  LOUT1 ──────┼────────┤├─────────────┬───────────┼── VIN            │
  │  (line 0)    │        100nF          │           │   (60 kOhm,      │
  │              │     (was 10nF --      │           │    AC-coupled)   │
  │              │      265 Hz corner    │           │                  │
  │              │      against 60k,     │           │      TIP ────────┼──► RJ12
  │              │      inside the       │           │      RING ───────┼──► RJ12
  │              │      voice band)      │           │                  │
  │  LIN1  ──────┼────────┤├──────┬──────┼───────────┼── VOUT           │
  │  (ADC in)    │       100nF  [Rdiv]   │           │   (10 Ohm out)   │
  └──────────────┘                │      │           └──────────────────┘
                                 GND     │
                                         │  ◄── shared AC-only node,
                                         │      sits at ~3.9 V DC (the
                                         │      SLIC's own Vin reference)
                                         │
                     R_prot 1k           │
  BM64          ┌───/\/\/\───────────────┘
  (module)      │
                │     ┌──────────┬──────────┐
                │     │          │          │
                │    ─┴─ D1     ─┴─ D2      │  ← clamp at the Vin tap
                │    /_\        /_\         │
                │     │          │          │
                │   +5 V        GND         │
                │                           │
                │        C_bm64             │
  AOHPL (pin 7) ┴────────┤├─────────────────┘
                        10uF
```

**Clamp to +5 V, not +3.3 V** -- Vin idles at ~3.9 V DC (measured), so a
diode to a 3.3 V rail would forward-bias continuously and short the audio
away. The SLIC runs from +5 V, which is the rail to clamp to.

**A standard silicon diode is the right choice here, better than a Schottky.**
Counter-intuitive, but: with Vin biased at 3.9 V and the loudest measured
signal 2.35 Vpp, the positive peak reaches ~5.1 V. A 1N4148/BAV99 (Vf ~0.7 V)
starts conducting above ~5.7 V, comfortably clear of that peak. A Schottky
(Vf ~0.3 V) would conduct from ~5.3 V and clip the top of a loud signal. The
GND-side diode never conducts in normal operation at all (the negative peak
only swings down to ~2.7 V) -- it's purely there for faults.

Why each part, and what happens without it:

| Part | Value | Purpose | Failure mode if omitted |
|---|---|---|---|
| `C_bm64` | 10 uF | Blocks AOHPL's DC bias (~1.5 V measured) from the Vin node | DC bias fights the SLIC's own Vin reference |
| **`R_prot`** | **1 k** | **Limits inrush during hot-plug and fault current into Vin** | **A charged cap dumps unlimited current into Vin — the prime suspect for the dead SLIC** |
| **`D1`/`D2`** | **BAV99 / low-C dual** | **Clamps anything outside the local rails** | **Transients and tip/ring bridging reach Vin unclamped** |
| `C_codec` | 100 nF | AC-couples the codec's DAC | 10 nF puts the corner at 265 Hz, inside the voice band |

`R_prot` at 1 k against Vin's 60 kOhm costs ~0.14 dB, so it's effectively
free. **The series R and the diodes are not redundant** -- diodes bound the
voltage but do nothing about the cap's inrush current, and the resistor
limits current but can't stop a sustained overvoltage. Both, or neither
helps much.

### Considered and rejected: optocoupler isolation

Tempting (it would make the damage mechanisms structurally impossible) but
the wrong tool here:

- **Cheap optocouplers are switching devices, not linear ones.** Feeding
  line-level audio through a standard phototransistor coupler produces gross
  distortion -- the transfer curve isn't remotely linear. Passing audio needs
  a *linear* optocoupler (IL300-class, with a servo photodiode and a feedback
  amplifier around it), which is several dollars plus an op-amp and its own
  bias network on each side, versus cents for a resistor and a diode pair.
- **There's no isolation barrier to maintain.** The BM64 and the SLIC already
  share this board's ground and its 3.3/5 V rails. Galvanic isolation earns
  its cost across a real domain boundary -- tip/ring is one, and that's why
  the line side already has bridge diodes and a TVS. The AOHPL-to-Vin hop is
  entirely inside one low-voltage domain.
- **It wouldn't address the actual failure modes any better.** The suspects
  are inrush from hot-plugging, transients through the cap, and a possibly-
  open ground return -- all of which a series resistor plus clamp diodes
  handles directly, at the tap, for pennies.

So: **series resistor + standard silicon diodes.** The cost instinct and the
engineering agree for once.

**Open questions for next session:**

- **Power sequencing:** does the BM64 need to come up before/after the SLIC,
  or share a rail so they rise together? The damage mechanism above is
  really about hot-plugging rather than sequencing per se, but with both
  permanently wired on SMD5 it's worth confirming no ordering leaves AOHPL
  driving an unpowered Vin (or vice versa) -- check the BM64 datasheet's
  AOHPL output stage behavior during its own power-up/down.
- **Bench procedure until protection exists:** power down before rewiring;
  connect ground first and disconnect it last; never probe tip/ring and the
  Vin node in the same session without re-checking the probe ground.

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

## Full BT-as-cordless-handset plan (2026-09-26)

The goal, in the user's own words: **when a phone (iPhone or otherwise) is
connected over Bluetooth, this board's analog handset acts as that phone's
mic/speaker** -- lift the handset, it's the phone's audio; hang up, it's
released. Normal PSTN/VoIP telephony on that line is unavailable while a BT
device is actively connected (matches `bt_companion_line()`'s existing
design -- see `telephony.c`, confirmed working 2026-09-25). On top of the
audio-passthrough piece already built, this section works out the two
pieces still missing: **incoming-call ringing** and **outgoing dial**, plus
a real gap found while writing this up.

This all rides on the standard Bluetooth **Hands-Free Profile (HFP)** --
BM64 implements the AT-command side of HFP internally and exposes it to the
host MCU as simple UART events/commands (confirmed real, not guessed --
`reference/BM64_arduino/src/BM64_enum.h`), so none of this needs any custom
iPhone app or new pairing work. It's the exact same mechanism a Bluetooth
car kit or AirPods already use to ring/answer a call.

### Incoming call: ring the bell, answer on pickup

1. **BM64 -> ESP32**: `BM64_EVENT_CALL_STATUS` (`0x02`) with value
   `INCOMING_CALL` (`0x02`) fires the moment the connected phone starts
   ringing (before the user does anything). `bt_companion.c`'s event
   handler doesn't currently branch on this event at all -- needs a new
   case.
2. **ESP32 -> handset**: call `ag1170_ring_start(line)` (same function
   normal PSTN incoming ringing already uses) and set `telephony_state`
   to `TEL_RINGING` (already exists, currently PSTN-only) or a new
   dedicated state if PSTN-incoming and BT-incoming ever need to be told
   apart in the UI/logs -- probably not worth a new state, the trigger
   (BT event vs. PSTN ring detect) is already distinguishable at the call
   site.
3. **User lifts handset** (`on_offhook()`, `telephony.c`): today this
   function's very first check is `if (line == bt_companion_line() &&
   bt_companion_enabled())` -> `enter_bt_passthrough(line)`. That's exactly
   right for this case -- picking up *is* answering -- but it needs one
   more thing alongside entering passthrough: send `BM64_MMI_ACCEPT_CALL`
   (`0x04`, as an `MMI_CMD` `0x02`) over UART so the phone's call actually
   connects (per HFP, it rings on the phone's own side too until answered
   from either end -- the handset lift alone doesn't answer anything on
   the phone's side without this).
4. **`ag1170_ring_stop(line)`** on that same pickup, same as any normal
   incoming call.
5. **Hang-up mid-call**: on-hook while `CALL_STATUS` is still
   `CALL_ACTIVE` (`0x04`) should send `BM64_MMI_FORCE_END_CALL` (`0x02`)
   so the phone call actually ends, not just the local audio tap.
6. **Caller declines to answer / hangs up before pickup**: `CALL_STATUS`
   returning to `IDLE` (`0x00`) while still on-hook should stop the ringer
   (`ag1170_ring_stop`) and return telephony state to whatever BT-idle
   looks like (see the gap below) -- a missed/aborted ring, not an error.

### Outgoing call: dial from the rotary phone

`BM64_MAKE_CALL_CMD` (`0x00`) takes the phone number as its literal ASCII
text parameter (confirmed in the reference library's `makeCall()` --
`sendCommand(BM64_MAKE_CALL_CMD, 0, (uint8_t*)number, len)`, no special
encoding). This means:

1. Off-hook while BT is connected and idle enters `TEL_BT_PASSTHROUGH`
   today (audio only, no dial collection at all in this state currently).
   Dialing needs its own path: reuse the existing pulse/DTMF digit
   collector (already built for PSTN/VoIP dialing) to gather digits while
   in this state, the same debounce/inter-digit-timeout logic, just a
   different destination for the finished number.
2. Once the collector decides dialing is complete (same
   timeout/terminator logic normal dialing already uses), send the
   collected digits as `BM64_MAKE_CALL_CMD`'s parameter instead of
   consulting the dialplan/VoIP dispatch.
3. Feedback while dialing: reuse dial tone (already playing, since we're
   off-hook) until the first digit, silence during collection (matches
   normal dialing's own UX) -- no new tone needed there.
4. Feedback on call result: `CALL_STATUS` -> `OUTGOING_CALL` (`0x03`) means
   it's ringing out -- could optionally play ringback tone
   (`TEL_RINGBACK` already exists for the PSTN/VoIP case) for
   consistency, though the phone's own earpiece-equivalent audio may
   already carry a ringback tone over the BT audio link itself, making a
   locally-generated one redundant or even doubled -- worth listening to
   a real test call before deciding whether to add this. `CALL_STATUS`
   never leaving `OUTGOING_CALL` (call not answered, or rejected) should
   time out to on-hook-equivalent + a busy/reorder tone (`TEL_BUSY` /
   `TEL_REORDER` already exist), matching how a failed PSTN/VoIP call
   already resolves.

### Gap found while writing this up: BT "enabled" vs. BT "connected"

`on_offhook()`'s routing check today is `bt_companion_enabled()` alone --
**not** whether a phone is actually connected. That means simply turning
the BT companion feature on (Settings -> Bluetooth) makes that line's
normal dial tone disappear entirely, even with no phone paired/in range at
all, any time the handset is lifted -- confirmed live 2026-09-25 (see the
top of this file's newer notes) that this exact condition can make a board
look completely broken ("no dial tone") when BT is simply sitting enabled
with nothing connected. This directly answers the "can we still make a
call if BT isn't actively sending audio" question: **yes, and it should be
the default** -- the routing check should be `bt_companion_enabled() &&
bt_companion_connected()` (a new or existing status field -- `bt_connected`
already exists in `bt_companion_status_t`, just needs threading through to
this check), so lifting the handset with BT enabled-but-idle falls through
to completely normal PSTN/VoIP dial tone, and only actually diverts to BT
passthrough when a phone is genuinely linked. This is a real bug to fix
independent of everything else in this section, not just a planning nice-
to-have.

**Fixed 2026-09-27.** `on_offhook()` now calls `bt_companion_should_intercept()`,
which checks `bt_companion_get_status().bt_connected` in addition to
`bt_companion_active()`. Found the hard way on newphone with the rebuilt
SLIC: `tone` played perfectly over the exact physical path a real off-hook
produced total silence on, because BT_PASSTHROUGH was intercepting the line
with Bluetooth enabled and fitted but nothing actually paired. Deliberately
scoped to the pickup moment only -- a live disconnect *during* an
already-answered BT_PASSTHROUGH call still doesn't drop back to dial tone
automatically until the next hang-up/pickup. Revisit if that turns out to
matter in practice.

### Open questions / out of scope for a first pass

- **Call waiting / second incoming call while already on a BT call**: HFP
  supports this (`BM64_MMI_ACCEPT_WAITING_HOLD_CALL_RLS_ACTIVE_CALL`
  exists in the reference enum) but a rotary phone has no way to signal
  "which call" -- probably just ignore/reject the second call for a first
  pass, revisit if it's ever actually annoying in practice.
- **Caller ID**: HFP provides it (`BM64_EVENT_CALL_ID`, `0x03`) but a
  rotary phone has no display -- genuinely nothing to do with this info
  today; noting it exists in case a future feature (spoken caller-name
  prompt via TTS, logged to the web UI activity feed) wants it.
- **Two-line boards**: this whole design already only applies to
  whichever line `bt_companion_line()` returns (1 on a 2-line board) --
  the other line keeps working as pure normal PSTN/VoIP telephony
  throughout, no interaction expected, but worth explicitly testing once
  this is built, not just assumed.
- **BM64 EEPROM HF-profile settings**: confirm `PROFILE_SETTING2`
  ("Support HF Profile") is actually enabled in whatever EEPROM config
  ends up shipped (see `BM64/eeprom_write/` for the current audio-fix
  work) -- A2DP-only audio passthrough (music) would keep working even if
  this got missed, but call ringing/answer/dial silently wouldn't, and
  that failure mode (BM64 just never emitting `CALL_STATUS` events at
  all) would look identical to a bug in this new code, not a config gap.

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
(Superseded by the fuller plan above, kept here since it was the original
seed of the idea and is still an accurate one-paragraph summary.)
