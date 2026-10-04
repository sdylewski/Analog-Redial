# SMD2 board (archived reference)

**Only 3 of these were ever built** (hand-soldered, bench-tested, long since
superseded by SMD4). Kept here as a single reference in case any of it is
needed again -- not part of the current build story, not linked from public
docs. Everything below was previously three separate files:
`ROTARY_PHONE_SMD.md` (design reference), `BOM-smd.md` (parts list), and
the SMD2-specific sections of `HARDWARE_SIGNAL_LEVELS.md` (a real DTMF-
clipping investigation with "a ton of rework" -- the resistor-divider
story below is exactly that).

KiCad project: `hardware/kicad/Rotary_phone_SMD2/`. Current firmware still
builds for this board (`CONFIG_BOARD_TYPE_SMD2`, `build.smd2/`) -- see
`CLAUDE.md`'s board-type build table -- this doc is the *hardware*
reference, not a statement that the board itself is unsupported.

### Rework checklist for a bare SMD2 board

Every SMD2 needs all of these before it works fully. Each one is detailed
further down.

1. **USB D+/D− swapped** between U3 (USBLC6) and the WROOM-1: **cut both
   original traces**, then cross-wire. Without this the board never shows up
   on USB. See [USB D+/D− swap](#known-bug-smd2-usb-dd-swapped-between-u3-and-the-module).
2. **SLIC Vin/Vout swapped** at the coupling caps (C16/C17, C18/C19), both
   lines: cut and cross. Without it no audio reaches the phone line. See
   [the first KNOWN BUG](#known-bug-smd2-confirmed-on-the-hand-soldered-board-also-present-in-smd3-schematic).
3. **Line 1 mic:** pull C24 and wire a 100 nF cap from line 1's AG1171
   Vout to C24's RIN1 pad, or line 1 is never heard during a call.
4. **DTMF clipping divider** on each SLIC→codec input (final V3 values in
   [Signal-level rework history](#signal-level-rework-history-dtmf-clipping-aec-timing)).
5. Confirm U1 (LDO) EN, pin 3, is tied to VIN.

No BOOT (J2) jumper is needed to flash over USB. The chip's built-in USB
port shows up even with blank flash, and esptool puts it in download mode
by itself. J2 is only for flashing through the UART pins (J3/J4).

---

## Rotary Phone Gateway — SMD board

Design reference for the **surface-mount** gateway PCB. This is the planned
production layout; the KiCad project is a work in progress.

| Feature | THT reference board | SMD board (this doc) |
|---------|---------------------|----------------------|
| KiCad project | `hardware/kicad/rotary_phone_gateway/` | `hardware/kicad/Rotary_phone_SMD2/` |
| MCU | ESP32-C6-DevKitM-1 (headers) | **ESP32-C6-WROOM-1** module (soldered) |
| SLIC | 2× AG1170 | 1× or 2× **AG1171** (SIL, hand-solder) |
| Passives | Through-hole | **0402 SMD** (JLC assembly) — see [KiCad symbols & footprints](#kicad-symbols--footprints-passives--support) |
| Codec | WM8731 SSOP-28 | **ES8388** QFN-28 (JLC **C365736** or hand-solder) |
| Extra storage | SPIFFS on flash only | Optional **microSD** (SPI) |

#### SMD package size: **0402** (JLC assembly)

This board is stuffed by **JLCPCB** economic assembly for **passives only** unless noted.
**Extended parts** cost **~$3 USD per unique LCSC part number** (reel loading fee) — see
[JLC vs Digikey sourcing](#jlc-vs-digikey-sourcing).

| Size | Use on this board |
|------|-------------------|
| **0402** | **Default** — all resistors; ceramics ≤ 1 µF (100 nF, 10 nF, etc.) |
| **0603** | **10 µF** ceramics (better part choice than 0402 at this value) |
| **0805** | **22 µF** bulk on 3.3 V (when WROOM support caps are added) |
| **THT radial** | **470 µF 16 V** at each AG1171 **+VPWR** (**C20** / **C21**) — not 220 µF (that was the old AG1170 THT board) |
| **SOD-123 / SMA / SMB** | 1N4148W (PD) / **M7** (bridge) / **BZT03C82** or **P6KE82** (line TVS) |

**Hand-solder at home (Digikey/Mouser):** **AP2112K-3.3** LDO, **USBLC6** USB ESD (Extended **SOT-23**), **WROOM-1**,
**AG1171** SIL, **RJ12**, **470 µF**, line **BZT03C82** or **P6KE82** TVS. **Let JLC place** USB-C, **ES8388**, **D1** SS14,
**M7** bridge diodes, passives, and **BOOT** header.

JLC placement cost is per component, not per package — **0402** saves a little BOM
cost and board area vs 0805 with no assembly downside.

Firmware pin map: `firmware/components/board/include/board_pins_smd.h`

Related: [the signal-level rework section below](#signal-level-rework-history-dtmf-clipping-aec-timing),
[`FEATURES.md`](../FEATURES.md). Codec reference (legacy, through-hole board): [`WM8731.md`](through-hole-wm8731/WM8731.md).
ES8388 datasheet (local): [`datasheets/ES8388.pdf`](../datasheets/ES8388.pdf)

Datasheet (local): [`datasheets/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf`](../datasheets/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf)  
Text extract: [`datasheets/ESP32-C6-WROOM-1_datasheet_extracted.txt`](../datasheets/ESP32-C6-WROOM-1_datasheet_extracted.txt)

---

---

### Designator map (schematic)

Matches **`Rotary_phone_SMD2.kicad_sch`** / **`Rotary_phone_SMD2.kicad_pcb`** (re-annotated).
Functional names in older sections (e.g. `C_LDO_IN`) are replaced by **actual refs** below.
**Update PCB from schematic** after annotation changes.

#### ICs, connectors, modules

| Ref | Value | Role |
|-----|-------|------|
| **U1** | **AP2112K-3.3** | 3.3 V LDO — hand-solder; JLC **C82942** DNP (alt **ME6211**) |
| **U2** | **ESP32-C6-WROOM-1** | MCU module |
| **U3** | **USBLC6-2SC6** | USB D+/D− ESD — hand-solder |
| **U4** | **ES8388** | Stereo codec |
| **U5** | **AG1171** | SLIC at **J6** (silk **Line 1**) — codec **LIN2** / **ROUT1** |
| **U6** | **AG1171** | SLIC at **J7** (silk **Line 2**) — codec **LIN1** / **LOUT1** — **DNP** build B |
| **USB1** | **TYPE-C 16PIN 2MD(073)** | USB-C receptacle (JLC) |
| **Card1** | **TF-01A** | microSD — **DNP** build A |
| **J6** | **RJ12** | Phone jack (silk Line 1) → **U5** |
| **J7** | **RJ12** | Phone jack (silk Line 2) → **U6** — **DNP** build B |
| **J5** | **1×3** header | Audio monitor **L / R / GND** (**LOUT2** / **ROUT2**) |
| **J2** | **1×2** | **Boot strap** — **GPIO9** + **GND** |
| **J3** | **1×2** | UART **TX** (**GPIO16**) + **GND** |
| **J4** | **1×2** | UART **RX** (**GPIO17**) + **GND** |
| **J8** | **2-pin 5 mm** terminal | **+5 V** / **GND** (optional bench power) |

GPIO net names **`Line1_*`** wire to **U6** / **J7**; **`Line2_*`** wire to **U5** / **J6** (firmware `Line1` = codec
left / **LIN1**, not necessarily the silk “Line 1” jack).

#### Diodes

| Ref | Value | Role |
|-----|-------|------|
| **D1** | **SS14** | VBUS → **+5 V** (cathode toward +5 V) — **JLC Basic** **C2480** |
| **D2** | Red LED | Status LED (**GPIO11** via **R6**) |
| **D3** | **1N4148W** | **U5** **PD** (GPIO **Line2_PD**) |
| **D4** | **1N4148W** | **U6** **PD** (GPIO **Line1_PD**) |
| **D5–D8** | **1N4007** (M7) | Tip/ring bridge at **J6** / **U5** |
| **D9–D12** | **1N4007** (M7) | Tip/ring bridge at **J7** / **U6** |
| **D13** | **BZT03C82** or **P6KE82** | Line TVS at **J6** / **U5** |
| **D14** | **BZT03C82** or **P6KE82** | Line TVS at **J7** / **U6** |

#### Resistors

| Ref | Value | Role |
|-----|-------|------|
| **R1** | **10 kΩ** | **U2** **EN** pull-up |
| **R2** | **10 kΩ** | **Card1** DAT2 (pin 1) pull-up — build B |
| **R3** | **10 kΩ** | **Card1** DAT1 (pin 8) pull-up — build B |
| **R4**, **R5** | **5.1 kΩ** | **USB1** **CC1** / **CC2** → GND |
| **R6** | **220 Ω** | **D2** LED (**GPIO11**) |
| **R7–R10** | **0 Ω** | XOR: **Line2** GPIO ↔ **Card1** SPI (populate one side only) |
| **R11**, **R12** | **4.7 kΩ** | **I²C** pull-ups (**SDA** / **SCL**) |
| **R13** | **10 Ω** | **U4** **AVDD** → **HPVDD** (optional) |
| **R14**, **R15** | **47 kΩ** | **J5** monitor bleed (**C14** / **C15** → GND) |
| **R16** | **15 kΩ** | **Line2_SHK** divider → GND (**U5**) |
| **R17** | **10 kΩ** | **U5** **SHK** → **Line2_SHK** tap |
| **R18** | **15 kΩ** | **Line1_SHK** divider → GND (**U6**) |
| **R19** | **10 kΩ** | **U6** **SHK** → **Line1_SHK** tap |

#### Capacitors — power & ESP32

| Ref | Value | Role |
|-----|-------|------|
| **C1** | **1 µF** | **U2** **EN** RC delay |
| **C2** | **10 µF** | **+5 V** bypass (LDO in / VBUS) |
| **C3** | **22 µF** | **+3.3 V** bulk |
| **C4**, **C5** | **100 nF** | **+3.3 V** bypass (module / rail) |

#### Capacitors — ES8388 (**U4**)

| Ref | Value | Role |
|-----|-------|------|
| **C6** | **1 µF** | **VREF** (pin 10) → **AGND** |
| **C7** | **1 µF** | **ADCVREF** (pin 19) → **AGND** |
| **C8** | **1 µF** | **VMID** (pin 20) → **AGND** |
| **C9**, **C10** | **100 nF** | **DVDD** / **PVDD** → **DGND** |
| **C11** | **10 µF** | **AVDD** bulk → **AGND** |
| **C12** | **100 nF** | **AVDD** → **AGND** |
| **C13** | **100 nF** | **HPVDD** → **HPGND** |
| **C14**, **C15** | **100 µF** | **ROUT2** / **LOUT2** → **J5** (AC coupling) |
| **C24** | **100 nF** | **RIN1** → **AGND** (SE reference) |
| **C25** | **100 nF** | **RIN2** → **AGND** (build A) |

#### Capacitors — SLIC (**U5** @ **J6**, **U6** @ **J7**)

| Ref | Value | Role |
|-----|-------|------|
| **C16** | **10 nF** | **U6** **VIN** ↔ **LIN1** |
| **C17** | **100 nF** | **U6** **VOUT** ↔ **LOUT1** |
| **C18** | **100 nF** | **U5** **VOUT** ↔ **ROUT1** |
| **C19** | **10 nF** | **U5** **VIN** ↔ **LIN2** |
| **C20** | **470 µF** | **U5** **+VPWR** bulk (THT) |
| **C21** | **470 µF** | **U6** **+VPWR** bulk (THT) |
| **C22** | **100 nF** | **+5 V** bypass at **U5** |
| **C23** | **100 nF** | **+5 V** bypass at **U6** |

---

### Signal flow (audio path)

Telephony audio moves **RJ12 → SLIC → codec → ESP32** on capture, and the reverse on
playback. Control (PD, ring, SHK) is GPIO beside this path.

```text
J6/J7 (RJ12) ◄──► U5/U6 (AG1171) ◄──► U4 (ES8388) ◄──I²S/I²C──► U2 (ESP32-C6)
     tip/ring        VIN/VOUT           LIN1/2 LOUT1/ROUT1

Debug playback tap (optional): U4 LOUT2/ROUT2 ──► J5 (headphones / scope)

Build B only:  U2 ◄──SPI──► Card1 (TF-01A)   (GPIO 18–21 — not with U6/J7)
```

#### KNOWN BUG (SMD2, confirmed on the hand-soldered board; also present in SMD3 schematic)

**`C16`/`C17` and `C18`/`C19` swap the SLIC's Vin/Vout relative to the codec.** Confirmed
via netlist trace and bench measurement (2026-07-19): codec `LIN1`/`LIN2` (ADC **inputs**,
passive) land on `U5`/`U6` pin **9 (Vin)** — the pin that needs an external driver — while
codec `LOUT1`/`ROUT1` (DAC **outputs**, actively driven) land on pin **10 (Vout)** — the
pin the SLIC itself drives. Backwards on both lines. This is why a 1kHz test tone
measures fine at the codec (J5, and at the coupling cap's codec-side pad) but never
reaches tip/ring.

**Bodge fix (no need to desolder the AG1171 SIL module):** cut the trace from each
coupling cap to the SLIC pin and cross them — audio bandwidth means wire length/routing
doesn't matter here, unlike the [USB D+/D- bodge](#known-bug-smd2-usb-dd-swapped-between-u3-and-the-module).
- Line 0: `C16` pin2 → `U6` pin 10 (was → pin 9); `C17` pin2 → `U6` pin 9 (was → pin 10)
- Line 1: `C19` pin2 → `U5` pin 10 (was → pin 9); `C18` pin2 → `U5` pin 9 (was → pin 10)

**Next spin (SMD3): swap the schematic net assignment** so `LOUT1`/`ROUT1` connect to
each SLIC's `Vin` and `LIN1`/`LIN2` connect to each SLIC's `Vout` — don't just carry the
bodge forward again.

**Power (not in audio path):** **USB1** → **+5 V** → **U1** → **+3.3 V** → **U2**, **U4**;
**+5 V** also feeds **U5/U6** **+VPWR**.

#### KNOWN BUG (SMD2): USB D+/D− swapped between U3 and the module

The PCB routes U3's D+ to the WROOM-1's D− pin (pin 13, GPIO12) and vice
versa. The board then never appears on USB.

**Bodge fix:** cut **both** traces between U3 and module pins 13/14, then
run two short wires crossing them over (U3's D+ side to pin 14/GPIO13, D−
side to pin 13/GPIO12). Keep the wires short and next to each other: this is
12 Mbit/s USB, unlike the audio bodges, where wire routing doesn't matter.

**If you add the wires but forget to cut the traces,** D+ and D− end up
shorted together. Symptom (2026-10-03, third board): 3.3 V and EN fine, but
**both** D+ and D− read about 2.9 V at the module and the Mac shows no USB
device at all. A healthy board reads D+ ≈ 3 V and D− ≈ 0 V while plugged in.
Cutting the old traces fixed it immediately.

#### KNOWN BUG (SMD2, fixed via bodge 2026-07-20): only one line's mic capturable at a time

**Symptom:** dialing line0 → line1 rang and connected fine, and line0's handset audio was
heard on line1's speaker, but line1's handset audio was never heard on line0's speaker
(whichever line's mic *wasn't* currently muxed onto the ADC was silent on the far end).

**Root cause:** `U4` (ES8388) was wired mono-in on this board — `LIN1` = line0 rx,
`LIN2` = line1 rx, sharing the *same* left ADC channel via the `LINSEL` mux
(`ADCCONTROL2`), while `RIN1`/`RIN2` (the independent right ADC channel) were dead —
just a 100nF bypass cap (`C24`/`C25`) to GND, no real source. The ES8388's left and right
ADC channels are otherwise fully independent and simultaneous (confirmed against the
datasheet block diagram, §7-8) — this was a wiring choice, not a chip limitation.
Firmware set `LINSEL` once at boot (defaulting to line0) and never changed it during a
call, so line1's mic never reached the ADC during a bridge.

**Bodge fix (no IC rework needed):** pull `C24` (100nF, right next to `U4`, normal 0402
footprint) and wire a new 100nF coupling cap from line1's AG1171 `Vout` pin to `C24`'s
now-vacant pad (`RIN1`). `LIN1`/`LIN2`/`C16`/`C19` are untouched — line1's old tap into
`LIN2` is simply never selected by `LINSEL` again. Since `LINSEL`/`RINSEL` are
independent single-ended muxes (not tied to a matching L-R pair — that constraint only
applies to the chip's *differential* input mode, which isn't used here), no bias
resistor network is needed either; a plain series coupling cap matches how `LIN1`/`LIN2`
were already done. Firmware: `es8388_init()` now fixes `LINSEL=00`/`RINSEL=00`
permanently (`firmware/components/es8388/es8388.c`) — no runtime mux-switching, both
lines' rx now digitize simultaneously every I2S frame.

**Gain had to be tuned down — turned out to be feedback, not the bodge wire being
noisy.** First hypothesis: the hand-soldered `RIN1` wire picks up more noise than a PCB
trace, and at the original PGA gain (+24dB, `MicAmpL`/`MicAmpR` in `ADCCONTROL1`, both
channels previously shared one value) the right channel's noise floor amplified into
near-saturation (RMS ~27000/32767 at idle). Confirmed the coupling cap itself was fine
first (DC voltage measured significantly lower on the codec side of the cap than the
SLIC side — real AC coupling, not a short or dead part). Cutting right-channel gain to
+12dB (leaving left at the original ~+24dB) did clean up that specific symptom — but a
live two-line call still produced loud static on **both** handsets, and it visibly
**ramped up over ~0.5s to full volume** rather than being present at fixed amplitude —
the signature of a positive-feedback loop (loop gain > 1), not a fixed noise source. Make
sense in hindsight: before this bodge, a full duplex loop through both handsets'
speakers+mics was *structurally impossible* (only one ADC channel was ever capturing),
so nothing had ever exercised whatever acoustic (handset earpiece-to-mouthpiece) or
electrical (AG1171 sidetone) coupling exists between the two lines at real gain. Dropping
**both** channels to +3dB (`ADCCONTROL1 = 0x11`) broke the loop — clean, clear two-line
call. Tuned empirically via the bring-up console's `micamp <left> <right>` command
(added for this debugging session) before being baked into `es8388_init()` as the
permanent default.

**Next spin (SMD3): route this properly** — LIN1/RIN1 (or whichever pins are cleanest in
layout) as each line's dedicated mic input from the start, instead of carrying the bodge
forward.

---

### Power — USB-C & 3.3 V regulator (U1, USB1, U3)

**5 V in** from **USB-C** feeds the **3.3 V LDO** and a separate **+5 V** branch for the SLICs.
Program/debug over **USB 2.0** on ESP32 **GPIO12/13** (see [ESP32 module](#esp32-c6-wroom-1-module-u2)).

```text
USB1 (VBUS) ──[D1 SS14]──► +5V ──┬──► U1 VIN ──► +3.3V ──► U2, U4, pull-ups, Card1
                                 │         ▲
                                 │         └── U1 EN/CE tied here (always on)
                                 └──► +5V ──► U5/U6 +VPWR (SLIC)

USB1 D± ──► U3 (USBLC6) ──► U2 GPIO12/13
USB1 CC1/CC2 ── 5.1k ── GND          USB1 EH (13/14) ── GND
```

#### 3.3 V LDO (**U1**)

| Item | Choice |
|------|--------|
| **Primary** | **AP2112K-3.3TRG1** — 3.3 V, 600 mA, SOT-23-5, low dropout |
| **JLC** | **C82942** (Extended — **DNP**, hand-solder from DigiKey) |
| **Footprint** | `JLCImport:ME6211C33M5G-N` (imported from LCSC; matches **C82942** pinout) |
| **Alt (drop-in)** | **ME6211C33M5G-N** — same SOT-23-5 pinout, 500 mA |

**Why AP2112K as default:** pin-compatible with the **C82942** land pattern; order
**AP2112K-3.3TRG1CT-ND** on DigiKey. **ME6211** is a valid substitute if you already have it.

**Not used:** **1.8 V LDO** (**U_LDO18**) — ES8388 runs from **3.3 V** only. See
[legacy WM8731](#legacy-wm8731-reference).

| Ref | Part | Value / notes | Package |
|-----|------|----------------|---------|
| **U1** | LDO | **AP2112K-3.3** (alt **ME6211C33M5G-N**) — **hand-solder**, DNP on JLC | SOT-23-5 |
| **C2** | Cap | **10 µF** on **+5 V** / LDO **in** | **0402** |
| **C11** | Cap | **10 µF** on **+3.3 V** at LDO **out** | **0402** |
| **C3** | Cap | **22 µF** **+3.3 V** bulk near LDO / WROOM | **0603**† |

##### LDO enable pin — **EN** (AP2112) / **CE** (ME6211)

This is the **regulator** enable input on **U1 pin 3**. It is **not** the ESP32 module **EN**
(reset) pin — that is **R1** + **C1** on **U2** (see [ESP32 module](#esp32-c6-wroom-1-module-u2)).

**Wire pin 3 to VIN (pin 1)** on the same **+5 V** net so the LDO is **always on** when USB
power is present. No extra resistor needed.

| Pin | ME6211 | AP2112K | Connect to |
|-----|--------|---------|------------|
| **1** | **VIN** | **VIN** | **+5 V** (after **D1**) |
| **2** | **VSS** | **GND** | **GND** |
| **3** | **CE** | **EN** | **VIN** (tie to pin **1** — active high) |
| **4** | **NC** | **NC** | **Leave unconnected** |
| **5** | **VOUT** | **VOUT** | **+3.3 V** rail |

```text
+5V ──┬── pin 1  VIN
      └── pin 3  EN / CE        ← same net; do not float

pin 2  GND / VSS ── GND
pin 4  NC          (no connect)
pin 5  VOUT ── +3.3V + 10µF + 22µF bulk
```

- **Do not** leave **EN/CE** floating — the LDO may not start reliably.
- **Do not** tie **EN/CE** to **GND** unless you intend to keep 3.3 V off.
- GPIO control of **EN** is optional and not used on this board.

**Schematic check:** if ERC shows **U1-EN** unconnected, add a wire **EN → VIN** before fab.

#### USB-C receptacle (**USB1**)

| Field | Value |
|-------|--------|
| **Part** | SHOU HAN **TYPE-C 16PIN 2MD(073)** |
| **LCSC / JLC** | **C2765186** (Extended — let JLC place) |
| **Pins used** | VBUS, GND, D+, D−, CC1, CC2; **EH (13/14)** → **GND** (shell pegs) |
| **Unused / NC** | SBU1/2, Dp2/Dn2 (USB3 pairs) |

| Ref | Part | Value / notes | Package |
|-----|------|----------------|---------|
| **USB1** | Connector | **TYPE-C 16PIN 2MD(073)** **C2765186** | SMD 16P |
| **D1** | Diode | **SS14** — VBUS → +5 V (**JLC Basic** **C2480**); cathode toward +5 V | SMB |
| **R4**, **R5** | Resistor | **5.1 kΩ** ×2 — **CC1** / **CC2** → GND | **0402** |
| **C2** | Cap | **10 µF** on **+5 V** at connector / LDO in | **0402** |

#### USB ESD (**U3**)

**USBLC6-2SC6** between **USB1** and ESP32 **GPIO12/13** — **hand-solder** (Extended SOT-23-6;
DNP on JLC). DigiKey [USBLC6-2SC6CT-ND](https://www.digikey.com/en/products/detail/umw/USBLC6-2SC6/16705896) (UMW) or JLC **C7519** if you prefer JLC to place it.

Discrete TVS on D+/D− is a poor substitute (capacitance / matching).

#### Power rails (whole board)

```text
USB-C VBUS (5V) ──┬──[SS14 D1]──► +5V ──┬──► U1 (AP2112K) ──► +3.3V ──► U2, U4, I²C, microSD
                  │                    │
                  │                    └──► +5V branch ──► U5/U6 +VPWR (SLIC)
                  └── GND (incl. USB EH pegs 13/14)
```

| Rail | Loads |
|------|--------|
| **+3.3 V** | **U2** WROOM, **U4** ES8388 (all codec supplies), **Card1**, GPIO pull-ups |
| **+5 V** | **U5/U6** AG1171 **+VPWR** — wide trace from VBUS, not from LDO |
| **GND** | Common; **DGND/AGND/HPGND** at codec tie here locally (see ES8388 section) |

**5 V SLIC + 3.3 V ESP32:** AG1171 **F/R** and **RM** may need **5 V**-tolerant drive — see
[SHK / GPIO notes](#shk-level-divider-5v--esp32-gpio) in the SLIC section.

---


### ESP32-C6-WROOM-1 module (U2)

Use a **certified Espressif module**, not a bare ESP32-C6 chip.

| Item | Choice |
|------|--------|
| **Part** | **ESP32-C6-WROOM-1-N16** (16 MB flash, no PSRAM) |
| **Alternates** | **-N8** (8 MB) minimum; **-N4** (4 MB) tight once VoIP + assets land |
| **Antenna** | **-WROOM-1** = on-module **PCB antenna** (default); **-WROOM-1U** = **U.FL** only (external antenna) — different module |
| **Footprint** | `PCM_Espressif:ESP32-C6-WROOM-1` or `Scott:ESP32-C6-WROOM-1` |
| **Mounting** | Castellated pads — solder flat on PCB (flux + fine tip or hot air) |

#### Inside the module vs on your PCB

The datasheet **Figure 8** (module block diagram) shows what Espressif puts **inside**
the certified module. **You do not add these on your board:**

| Inside WROOM-1 module | On your PCB? |
|----------------------|--------------|
| ESP32-C6 silicon | No — inside module |
| **40 MHz crystal** | No |
| **SPI flash** (4/8/16 MB) | No |
| **RF matching** (e.g. **2.0 nH** inductor, π-network) | No |
| **PCB antenna** (-WROOM-1) or **U.FL** (-WROOM-1U) | No (keep **keepout** clear) |
| 32 kHz crystal (optional in some designs) | No — marked **NC** in Fig. 9-1 |

The **Figure 9-1 “Peripheral Schematics”** page is what **your** board implements
around the module pins (power/USB in [Power section](#power--usb-c--33-v-regulator-u1-usb1-u3)).

#### Minimum application circuit (Figure 9-1)

From datasheet §9 + §4.5 (power-up / reset):

```text
+3.3V (from U1) ──┬── C3 22µF ── GND
                  ├── C4 100nF ── GND
                  └── 3V3 (pin 2) ── ESP32-C6-WROOM-1

              10k (R1) ── 3.3V
               │
              EN (pin 3) ── C1 1µF ── GND   (RC delay)

GPIO9 (pin 15) ── weak pull-up (inside module)
     │
   [J2] ── GND                     (boot strap — see [Boot strap](#boot-strap-gpio9))

GPIO12/13 ── USB D−/D+ ── (via U3) ── USB1

EPAD (pin 29) ── GND optional (recommended for thermal; do not over-paste)
```

**EN / reset (ESP32 module only):** The **WROOM EN** pin (module pin 3) is separate from **U1 EN/CE**.
Do not leave **WROOM EN** floating. Use **10 kΩ** pull-up to 3.3 V and **~1 µF** from EN to GND
for power-up delay (datasheet recommends **R = 10 kΩ, C = 1 µF**). **No RESET header** —
power-cycle or USB reset is enough.

**Boot / download:** Default is **SPI boot** (GPIO9 = 1 at reset). For firmware
download, strap **GPIO9 low** at reset (Table 4-3). Use **J2** (1×2) + jumper cap.

**Optional:** UART **J3** (**GPIO16** TX) / **J4** (**GPIO17** RX); **JTAG** if USB-JTAG disabled.

**Not needed (NC in Fig. 9-1):** 32.768 kHz crystal on IO0/IO1, extra flash, RF inductors.

#### Suggested BOM — ESP32 support (your PCB)

| Ref | Part | Value / notes | Package |
|-----|------|----------------|---------|
| **U2** | Module | **ESP32-C6-WROOM-1-N16** — **hand-solder** | WROOM castellated |
| **C4** | Cap | **100 nF** at module **3V3** pin | **0402** |
| **C1** | Cap | **1 µF** on **EN** (RC delay) | **0402** |
| **R1** | Resistor | **10 kΩ** **EN** pull-up | **0402** |
| **J2** | Header | **2.54 mm** 1×2 — pin **1** = **GPIO9**, pin **2** = **GND** + jumper | THT |
| **D2** | LED | Red status on **GPIO11** | **1206** |
| **R6** | Resistor | **220 Ω** — GPIO11 → LED | **0402** |

Keep **module antenna area** clear per datasheet §11 (~15 mm keepout).

#### Internal flash partitions (target, N16)

| Partition | Size (planned) | Use |
|-----------|------------------|-----|
| `factory` (app) | ~1.5 MB | Firmware + SIP stack headroom |
| `storage` (SPIFFS) | ~2–4 MB | Prompts, small clips |
| `nvs` | 24 KB | Wi-Fi / settings |

Set `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` when using **-N16**.

Flash/debug primarily over **USB**; UART on GPIO16/17 remains optional for bring-up.

---


### Build options (same PCB, different stuffing)

One layout supports **two product variants**. GPIO **18–21** are routed to
**line 2 SLIC** and to the **microSD socket** on the **same nets** — populate
**only one** peripheral on those pins.

```
ESP32 GPIO18 ──┬── U5 PD (line 2 SLIC)
               └── microSD CS
               … (19–21 likewise)
```

| Build | Populate | Do not populate (DNP) | Firmware |
|-------|----------|------------------------|----------|
| **A — Dual line** | U5, **U6**, J6, **J7**, both SLIC circuits | **Card1** DNP | `CONFIG_BOARD_NUM_LINES=2`, `CONFIG_BOARD_MICROSD=0` |
| **B — Single line + SD** | **U5**, **J6**, **Card1**, **U5** passives | **U6**, **J7**, **U6** passives | `CONFIG_BOARD_NUM_LINES=1`, `CONFIG_BOARD_MICROSD=1` |

**Never stuff U6/J7 and Card1 together** — they share GPIO 18–21.

Optional **4× 0 Ω** resistors (line-2 side *or* SD side, one set only) add
BOM clarity; shared copper without 0 Ω is fine if stuffing discipline is clear.

Silkscreen suggestion: **`LINE2 XOR microSD`**.

#### ASCII — GPIO 18–21 XOR routing

**Build A (dual line)** — populate U6, J7; DNP microSD:

```text
                    ┌── U6 (AG1171) ── J7 (RJ12 — schematic **Line1_***, firmware line 0)
                    │      PD   SHK   RM   F/R
ESP32-C6-WROOM-1    │
 GPIO18 ─────────────┤ PD
 GPIO19 ─────────────┤ SHK
 GPIO20 ─────────────┤ RM
 GPIO21 ─────────────┤ F/R
                    │
 microSD socket     └── (DNP — pads may be present, no IC stuffed)
```

**Build B (single line + SD)** — populate microSD; DNP U6, J7:

```text
                    ┌── U6 / J7 (DNP)
                    │
ESP32-C6-WROOM-1    │
 GPIO18 ─────────────┼── microSD DAT3 (CS)
 GPIO19 ─────────────┼── microSD CLK
 GPIO20 ─────────────┼── microSD CMD (MOSI)
 GPIO21 ─────────────┼── microSD DAT0 (MISO)
                    │
 U5 / J6 (line 2)     └── GPIO 1–3, 10 only  [U6 / J7 DNP — those GPIOs unused in build B]
```

**Optional 0 Ω stuffing pairs** (one set populated per build):

```text
Build A:  GPIO18 ──[R7 0Ω]── U5.PD          Build B:  GPIO18 ──[R7 0Ω]── SD.CS
          (DNP SD branch / R8–R10)                  (DNP U5.PD branch / R9–R10)
```

#### Firmware: `Line1_SHK`, 0 Ω stuffing, and microSD

**Line1_SHK** (**GPIO1**, schematic net to **U6** **SHK** via **R19**/**R18**) must be configured as a **GPIO input** during bring-up (see `board_line_pins(0).shk` in `board_pins_smd.h`). The **10 kΩ / 15 kΩ divider** pulls that pin **low when on-hook** (SLIC **SHK** low) and **~2.7–3.0 V** when off-hook.

GPIO **18–21** are a separate XOR: **R7–R10** route them to **Line 2 SLIC** (build A) **or** **Card1** SPI (build B), not both. If the **0 Ω resistors on the microSD branch** are populated (**R7–R10** toward **Card1**), those pins belong to SDIO — **do not initialize the microSD stack** when the board is actually stuffed or behaving as **dual-line** (both SLICs active).

| Situation | Firmware action |
|-----------|-----------------|
| Build A (dual line, 0 Ω on Line 2 side) | `CONFIG_BOARD_MICROSD=0` — **Card1** DNP; no SD driver |
| Build B (single line + SD, 0 Ω on SD side) | `CONFIG_BOARD_MICROSD=1` — **U6**/**J7** DNP; GPIO18–21 = SPI SD only |
| Mismatch (e.g. SD 0 Ω stuffed but **Line1_SHK** shows live SLIC / divider) | **Disable microSD** — wrong BOM or wrong `sdkconfig` |

**How to enforce (pick one or combine):**

1. **Build-time (recommended)** — Set `CONFIG_BOARD_NUM_LINES` and `CONFIG_BOARD_MICROSD` in `menuconfig` to match the stuffed build (see table above). No runtime guesswork.
2. **Boot-time probe (optional)** — Early in `app_main` (before any SDIO/SPI SD init), configure **Line1_SHK** as input, sample the divider level, and if it indicates **U6** is wired (on-hook idle → reads low) while **CONFIG_BOARD_MICROSD=1**, skip SD mount and log a warning. Same idea applies to **Line2_SHK** on **GPIO19** when build A 0 Ω is on the SLIC side.
3. **Configuration routine** — A small `board_detect_stuffing()` (or extend `board_i2c_init()` / board init) can cache “SD allowed” vs “dual-line SLIC” once at startup; telephony and storage subsystems consult that flag.

**Do not** use **GPIO9** (**J2** boot strap) for this check — it is for download mode only.

See `firmware/components/board/include/board_pins_smd.h` and [SHK level divider](#shk-level-divider-5v--esp32-gpio) above.

---


### GPIO pinout (ESP32-C6-WROOM-1)

#### Module pin connection table (U2)

Use this when wiring **U2** after removing the DevKitM-1. **Module pin numbers** are the
castellated pad order from the Espressif datasheet / KiCad `Scott:ESP32-C6-WROOM-1`
symbol — not the same as GPIO numbers.

**Important:** **GPIO14 is not exposed** on the WROOM-1 module (it was on the DevKit
header). **Line 1 PD** moves to **GPIO10** (module pin **11**). Update net `Line1_PD`
accordingly.

##### Power, reset, USB (U1 support — add to schematic)

| Mod pin | Symbol name | Connect to |
|---------|-------------|------------|
| **1** | GND | Board **GND** (multiple pads) |
| **2** | 3V3 | **+3.3 V** rail (LDO out + 22 µF ∥ 0.1 µF) |
| **3** | EN/CHIP_PU | **10 kΩ** → 3.3 V, **1 µF** → GND (**no RESET header** — power-cycle OK) |
| **13** | GPIO12/USB_D− | USB-C **D−** (optional ESD before connector) |
| **14** | GPIO13/USB_D+ | USB-C **D+** |
| **29** | EPAD | **GND** (recommended, light paste) |
| — | USB VBUS | **5 V** input to 3.3 V LDO (not a module pin) |
| — | USB CC1/CC2 | **5.1 kΩ** each → GND |

##### Signals — match existing schematic net labels

| Mod pin | GPIO | Schematic net | Connects to |
|---------|------|---------------|-------------|
| **8** | **0** | `I2S_MCLK` | ES8388 pin **1** (MCLK) |
| **4** | **4** | `I2S_BCLK` | ES8388 pin **5** (SCLK) |
| **5** | **5** | `I2S_LRCK` | ES8388 pin **7** (LRCK) |
| **6** | **6** | `I2S_DOUT` | ES8388 pin **6** (DSDIN → playback) |
| **7** | **7** | `I2S_DIN` | ES8388 pin **8** (ASDOUT ← capture) |
| **20** | **22** | `I2C_SDA` | ES8388 pin **27** (CDATA) + **R11** 4.7 kΩ → 3.3 V |
| **21** | **23** | `I2C_SCL` | ES8388 pin **28** (CCLK) + **R12** 4.7 kΩ → 3.3 V |
| **9** | **1** | `Line1_SHK` | **U6** **SHK** via **R19** (10 kΩ) + **R18** (15 kΩ) → GPIO |
| **27** | **2** | `Line1_F/R` | **U6** F/R — output |
| **26** | **3** | `Line1_RM` | **U6** RM — output |
| **11** | **10** | `Line1_PD` | **U6** PD via **D4** |
| **17** | **19** | `Line2_SHK` | **U5** **SHK** via **R17** (10 kΩ) + **R16** (15 kΩ) → GPIO (build A) |
| **19** | **21** | `Line2_F/R` | **U5** F/R (build A) |
| **18** | **20** | `Line2_RM` | **U5** RM (build A) |
| **16** | **18** | `Line2_PD` | **U5** PD via **D3** (build A) |
| **25** | **16** | *(optional)* `UART_TX` | **J3** (GPIO16) |
| **24** | **17** | *(optional)* `UART_RX` | **J4** (GPIO17) |

**Build B (microSD):** pins **16–19** → `SD_CS` / `SD_CLK` / `SD_MOSI` / `SD_MISO`
instead of line-2 SLIC nets; **DNP U6/J7**.

##### Strapping — do not tie as GPIO outputs

| Mod pin | GPIO | Notes |
|---------|------|-------|
| **10** | **8** | Boot strap — leave default (floating / pull-down) |
| **15** | **9** | Boot strap — **J2** pin 1; jumper to GND = download mode |
| **23** | **15** | JTAG strap — leave default for USB-JTAG |
| **12–13** | **12–13** | USB — see above |

##### Spare module pins

| Mod pin | GPIO | Suggested use |
|---------|------|---------------|
| **12** | **11** | `STATUS_LED` — **GPIO11** → **R6** → **D2** → **GND** (active-high) |

##### NC

| Mod pin | Name |
|---------|------|
| **22** | NC — do not connect |

#### GPIO quick reference (firmware = `board_pins_smd.h`)

##### Always connected

| GPIO | Function | Connects to |
|------|----------|-------------|
| **0** | I²S **MCLK** | ES8388 pin **1** — 12.288 MHz target (from ESP32 GPIO0) |
| **4** | I²S **BCLK** | ES8388 pin **5** (SCLK) |
| **5** | I²S **LRCK** | ES8388 pin **7** (LRCK) |
| **6** | I²S **DOUT** | ES8388 pin **6** (DSDIN) — playback to phone |
| **7** | I²S **DIN** | ES8388 pin **8** (ASDOUT) — capture from phone |
| **22** | **I²C SDA** | ES8388 pin **27** (CDATA) + **R11** 4.7 kΩ → 3.3 V |
| **23** | **I²C SCL** | ES8388 pin **28** (CCLK) + **R12** 4.7 kΩ → 3.3 V |
| **1** | Line 1 **SHK** | **U6** / **J7** — **R19** (10 kΩ) + **R18** (15 kΩ) divider |
| **2** | Line 1 **F/R** | **U6** F/R — output |
| **3** | Line 1 **RM** | **U6** RM — output |
| **10** | Line 1 **PD** | **U6** **PD** via **D4**; module pin 11 (**not** DevKit GPIO14) |
| **16** | *(optional)* `UART_TX` | **J3** (**GPIO16**) |
| **17** | *(optional)* `UART_RX` | **J4** (**GPIO17**) |
| **12** | **USB D−** | USB-C (programming) |
| **13** | **USB D+** | USB-C (programming) |
| **11** | Status **LED** | **GPIO11** → **R6** (**220 Ω**) → **D2** anode → cathode → **GND** (active-high) |

ES8388 I²C address: **0x10** (7-bit) with **CE** (pin **26**) tied to **GND**. Tie **CE** to **3.3 V**
for **0x11** if a second codec is ever added.

See [ES8388 support](#es8388-support-codec) for the full pin table and passives BOM.

#### GPIO 18–21 — dual-line **or** microSD (pick one build)

| GPIO | Build A (line 2) | Build B (microSD SPI) |
|------|------------------|------------------------|
| **18** | Line 2 **PD** | **SD CS** (DAT3) |
| **19** | Line 2 **SHK** (via **10 kΩ** / **15 kΩ** divider) | **SD CLK** |
| **20** | Line 2 **RM** | **SD MOSI** (CMD) |
| **21** | Line 2 **F/R** | **SD MISO** (DAT0) |

Line 1 (schematic **`Line1_*`**, firmware line 0): **U6**, **J7**. Line 2 (**`Line2_*`**): **U5**, **J6**.

#### Do not use for GPIO

| GPIO | Reason |
|------|--------|
| **8, 9, 15** | Boot / JTAG strapping (**GPIO9** = **J2** only — do not drive as output) |
| **12, 13** | USB (when USB port fitted) |

#### Spare

| GPIO | Notes |
|------|-------|
| *(none on WROOM-1)* | **GPIO10** = line 1 PD; **GPIO11** = status LED |

---


### ES8388 codec (U4)

The ES8388 is one **stereo** codec — two independent phone paths share **one I²S bus**.
Think in **line 1 / line 2** (product names), not “left ear / right ear.”

| Product name | I²S slot (ESP32 stereo) | ES8388 datasheet | Analog pins | SLIC | Jack |
|--------------|-------------------------|------------------|-------------|------|------|
| **Line 1** | **Left** (channel 0) | ADC **LIN1** / DAC **LOUT1** | **LIN1** in (+ **RIN1** → **AGND**), **LOUT1** out | **U6** | **J7** |
| **Line 2** (build A) | **Right** (channel 1) | ADC **LIN2** / DAC **ROUT1** | **LIN2** in (+ **RIN2** → **AGND**), **ROUT1** out | **U5** | **J6** |

**Why this is the right model**

- **Hardware:** one chip, one I²S cable (MCLK/BCLK/LRCK/DOUT/DIN) — already optimal vs two mono codecs.
- **Nomenclature:** firmware/docs say **line 1** / **line 2**; map once at the driver boundary:

  `LINE1 ↔ I2S_LEFT ↔ ES8388 LIN1/LOUT1`  
  `LINE2 ↔ I2S_RIGHT ↔ ES8388 LIN2/ROUT1`

- **ESP-IDF** still labels samples **left/right** in the I²S API — that is normal; your `audio` layer can expose `line_index` 0/1.

**Build B:** only **line 1** (left slot) is used — power down or mute **line 2** path in ES8388 registers; right I²S slot can be silent.

**Single-ended inputs:** route audio **only** to **LIN1** / **LIN2** (**10 nF** from **U5/U6 VIN**). **RIN1/RIN2** are the (−) side — tie to **AGND** (**100 nF** or direct strap). Do **not** connect SLIC audio to **RINx**.

**Line outputs** to SLIC **VOUT**: **100 nF** series on **LOUT1** / **ROUT1**.

**LOUT2** / **ROUT2**: optional **J5** monitor (same DAC channels as **LOUT1**/**ROUT1**).

I²S: **48 kHz**, **16-bit stereo** (one LRCK frame = line 1 sample + line 2 sample). MCLK = **12.288 MHz**
(256× Fs). Firmware: ES8388 driver with `BOARD_CODEC_CH_LINE1 = I2S_CHAN_LEFT`, etc.

---

---


**U4** — Everest **ES8388**, QFN-28-EP (4×4 mm). KiCad symbol: `Audio:ES8388`; footprint:
`Package_DFN_QFN:QFN-28-1EP_4x4mm_P0.45mm_EP2.6x2.6mm`. JLC **C365736** (~**$1** + **$3**
Extended). Pin-compatible family to WM8988; stereo I²S + I²C control.

#### ES8388 pin connection table (U4)

| Pin | Name | Connect to |
|-----|------|------------|
| **1** | **MCLK** | ESP32 **GPIO0** (`I2S_MCLK`) — 12.288 MHz from ESP32 I²S master |
| **2** | **DVDD** | **+3.3 V** + **100 nF** → **DGND** |
| **3** | **PVDD** | **+3.3 V** + **100 nF** → **DGND** |
| **4** | **DGND** | Digital ground (star with **AGND** / **HPGND** at codec) |
| **5** | **SCLK** | ESP32 **GPIO4** (`I2S_BCLK`) |
| **6** | **DSDIN** | ESP32 **GPIO6** (`I2S_DOUT` — MCU → codec playback) |
| **7** | **LRCK** | ESP32 **GPIO5** (`I2S_LRCK`) |
| **8** | **ASDOUT** | ESP32 **GPIO7** (`I2S_DIN` — codec → MCU capture) |
| **9** | **NC** | Leave **NC** |
| **10** | **VREF** | **1 µF** → **AGND** (reference decouple — close to pin) |
| **11** | **ROUT1** | **100 nF** (**C18**) → **U5** **VOUT** (line 2 DAC out, build A) |
| **12** | **LOUT1** | **100 nF** (**C17**) → **U6** **VOUT** (line 1 DAC out) |
| **13** | **HPGND** | **GND** (output-driver return) |
| **14** | **ROUT2** | Optional → **J5** **R** (monitor — see [Audio monitor](#audio-monitor-j5)) |
| **15** | **LOUT2** | Optional → **J5** **L** (monitor) |
| **16** | **HPVDD** | **+3.3 V** + **100 nF** → **HPGND**; optional **10 Ω** from **AVDD** if shared rail |
| **17** | **AVDD** | **+3.3 V** + **100 nF** + **10 µF** bulk → **AGND** |
| **18** | **AGND** | Analog ground |
| **19** | **ADCVREF** | **1 µF** → **AGND** |
| **20** | **VMID** | **1 µF** → **AGND** |
| **21** | **RIN2** | **100 nF** → **AGND** (line 2 ADC −, build A); **NC** on build B |
| **22** | **LIN2** | **10 nF** (**C19**) ← **U5** **VIN** (line 2 ADC +, build A) |
| **23** | **RIN1** | **100 nF** (**C24**) → **AGND** (line 1 ADC −) |
| **24** | **LIN1** | **10 nF** (**C16**) ← **U6** **VIN** (line 1 ADC +) |
| **25** | **NC** | Leave **NC** |
| **26** | **CE** | **GND** → I²C address **0x10** (7-bit); do **not** pulse low at reset (SPI select) |
| **27** | **CDATA** | ESP32 **GPIO22** (SDA) + **4.7 kΩ** → 3.3 V |
| **28** | **CCLK** | ESP32 **GPIO23** (SCL) + **4.7 kΩ** → 3.3 V |
| **EP** | Exposed pad | **GND** — multiple vias under pad |

**I²S mode:** slave; ESP32 is master. **No on-chip crystal** — MCLK from GPIO0 only.

#### ES8388 grounding (DGND / AGND / HPGND)

Everest user guide: **“Ground DGND, AGND, HPGND 0 V”** — same potential as board **GND**.
They are **not** separate isolated nets on this PCB.

| Pin | Role | What connects here |
|-----|------|-------------------|
| **DGND** (4) | Digital return | **DVDD** / **PVDD** bypass caps; I²S digital return |
| **AGND** (18) | Analog return | **AVDD** bypass + bulk; **VREF** / **VMID** / **ADCVREF**; **RIN1/RIN2** → **AGND** (reference) |
| **HPGND** (13) | Output-driver return | **HPVDD** bypass; return for **LOUT1/2** and **ROUT1/2** current |
| **EP** | Die pad | **GND** — several vias to ground plane |

**Tie all four to common board GND** under/near **U4** (short traces or ground pour + vias).
**CE** (26) and **J5** headphone sleeve also go to that same **GND** plane — do **not**
float the monitor header on a separate “headphone-only” ground.

**Why three GND pins if they’re all GND?** The chip splits returns inside the package so
digital switching noise does not ride on sensitive analog references. On the PCB you still
use **one ground plane**; the “which GND pin?” note on bypass caps only means **route each
cap’s ground terminal to the nearest matching pin** for the shortest loop, then stitch the
pins together at the codec.

```text
Board GND plane ──┬── DGND (4)  ◄── 100nF from DVDD, PVDD
                  ├── AGND (18) ◄── 100nF + 10µF from AVDD; 1µF from VREF/VMID/ADCVREF
                  ├── HPGND (13)◄── 100nF from HPVDD
                  └── EP (under pad)

+3.3V ── DVDD, PVDD, AVDD ──[10Ω opt]── HPVDD   (same rail; optional 10Ω isolates HPVDD ripple)
```

**J5 monitor header (LOUT2/ROUT2):** sleeve → **board GND** (same net as **HPGND** at codec).
Prefer a via/stitch near **HPGND**, not a separate ground island.

#### Suggested BOM — ES8388 passives & helper circuits

| Ref | Part | Value / notes | Package |
|-----|------|----------------|---------|
| **U4** | **ES8388** | JLC **C365736** or Digikey/LCSC | QFN-28 4×4 |
| **C9** | Cap | **100 nF** **DVDD** → **DGND** | **0402** |
| **C10** | Cap | **100 nF** **PVDD** → **DGND** | **0402** |
| **C12** | Cap | **100 nF** **AVDD** → **AGND** | **0402** |
| **C11** | Cap | **10 µF** **AVDD** bulk → **AGND** | **0402** |
| **C13** | Cap | **100 nF** **HPVDD** → **HPGND** | **0402** |
| **R13** | Resistor | **10 Ω** **AVDD** → **HPVDD** (optional) | **0402** |
| **C6** | Cap | **1 µF** **VREF** (pin 10) → **AGND** | **0402** |
| **C7** | Cap | **1 µF** **ADCVREF** (pin 19) → **AGND** | **0402** |
| **C8** | Cap | **1 µF** **VMID** (pin 20) → **AGND** | **0402** |
| **R11**, **R12** | Resistor | **4.7 kΩ** ×2 → **3.3 V** | **0402** |
| **C24** | Cap | **100 nF** **RIN1** → **AGND** | **0402** |
| **C25** | Cap | **100 nF** **RIN2** → **AGND** (build A) | **0402** |
| **C16** | Cap | **10 nF** **U6** **VIN** ↔ **LIN1** | **0402** |
| **C19** | Cap | **10 nF** **U5** **VIN** ↔ **LIN2** (build A) | **0402** |
| **C17** | Cap | **100 nF** **LOUT1** ↔ **U6** **VOUT** | **0402** |
| **C18** | Cap | **100 nF** **ROUT1** ↔ **U5** **VOUT** (build A) | **0402** |
| **C14**, **C15** | Cap | **100 µF** **LOUT2** / **ROUT2** → **J5** | **1206** |
| **R14**, **R15** | Resistor | **47 kΩ** bleed **C14** / **C15** → GND (optional) | **0402** |

**Not needed with ES8388:** **U_LDO18** (1.8 V), codec crystal, **MODE** / **CSB** straps.

```text
+3.3V ──┬── DVDD, PVDD, AVDD ──[10Ω opt]── HPVDD
        │     │ each 100nF to local GND; 10µF on AVDD
        │
        ├── 4.7k ──┬── GPIO22 ── CDATA (pin 27)
        │          └── 4.7k ──┬── GPIO23 ── CCLK (pin 28)
        │
GPIO0  ── MCLK  ── pin 1
GPIO4  ── BCLK  ── pin 5  (SCLK)
GPIO5  ── LRCK  ── pin 7
GPIO6  ── DOUT  ── pin 6  (DSDIN)
GPIO7  ── DIN   ── pin 8  (ASDOUT)

CE (26) ── GND          VREF/ADCVREF/VMID ── 1µF ── AGND

U6 VIN ── C16 10nF ── LIN1 (24)     RIN1 (23) ── C24 100nF ── AGND
U6 VOUT ◄── C17 100nF ◄── LOUT1 (12)

U5 VIN ── C19 10nF ── LIN2 (22)     RIN2 (21) ── C25 100nF ── AGND   [build A]
U5 VOUT ◄── C18 100nF ◄── ROUT1 (11)                              [build A]

LOUT2 / ROUT2 ── optional ──► J5 (monitor)
```

---


### AG1171 SLIC & RJ12 (U5, U6, J6, J7)

| Item | Detail |
|------|--------|
| **Symbol / footprint** | `Scott:AG1171_SIL` (14-pin SIL; same outline as AG1170 eval) |
| **Supply** | **5 V** on **+VPWR** from USB **VBUS** (this build) — wide trace + local bulk |
| **PD** | **1N4148W** (cathode toward PD pin); GPIO low = power-down, high-Z = run |
| **Pin 11** | **NC** on AG1171 — no +VA network (unlike AG1170) |
| **Coupling** | VIN **10 nF**, VOUT **100 nF** to ES8388 |
| **Bulk** | **470 µF** 16 V per SLIC (AG1171 Figure 4 **C2**) — **not** 220 µF (that value was on the old **AG1170** THT board) |

Mounting: **through-hole SIL** — hand-solder even on SMD board.

#### Suggested BOM — AG1171 support (per SLIC)

**U5** (**J6**): **C19**, **C18**, **C20**, **C22**, **D3**, **D5–D8**, **D13**, **R17**, **R16**  
**U6** (**J7**, build A): **C16**, **C17**, **C21**, **C23**, **D4**, **D9–D12**, **D14**, **R19**, **R18**

| Ref | Part | Value / notes | Package |
|-----|------|----------------|---------|
| **C20** / **C21** | Cap | **470 µF** 16 V on **+VPWR** — default **Rubycon 16YXJ470M8X11.5** | **THT radial** |
| **C22** / **C23** | Cap | **100 nF** on **+5 V** at module | **0402** |
| **C19** / **C16** | Cap | **10 nF** **VIN** ↔ codec **LINx** | **0402** |
| **C18** / **C17** | Cap | **100 nF** **VOUT** ↔ codec **LOUT/ROUT** | **0402** |
| **D3** / **D4** | Diode | **1N4148W** — cathode → **PD** | SOD-123 |
| **D13** / **D14** | TVS | **BZT03C82** or **P6KE82** on tip/ring | DO-41 THT |
| **D5–D8** / **D9–D12** | Rectifier | **1N4007** (M7) ×4 — bridge | **SMA** |
| **R17** + **R16** | Resistor | **10 kΩ** + **15 kΩ** — **U5** **SHK** divider | **0402** |
| **R19** + **R18** | Resistor | **10 kΩ** + **15 kΩ** — **U6** **SHK** divider | **0402** |

**470 µF cannot be 0402** — no practical ceramic at that value/voltage; use **THT radial**
next to the SIL module (JLC does not assemble the SLIC anyway). Optional SMD can
(`CP_Elec_8x10.5`) only if you want machine-placed bulk away from the SIL footprint.

**5 V at SLIC:** also place **0.1 µF** on **+5 V** at the SIL (**C22** / **C23**) within a few mm of the module pins.

**Do not use 0402 for:** 470 µF bulk, line TVS (SMB). **Use SMA** for **M7** bridge diodes (×4 per SLIC).
**Use 0402 for:**
10 nF, 100 nF coupling and bypass at the SLIC and codec, and **SHK divider** resistors.

#### SHK level divider (5 V → ESP32 GPIO)

AG1171 **SHK** can swing to **~4.5–5 V** when high; ESP32 GPIO is **not** 5 V-tolerant.
Use a resistor divider on each **SHK** line (line 1 and line 2 if populated):

```text
AG1171 SHK (SIL) ── 10kΩ ──┬──► ESP32 GPIO (Line1_SHK / Line2_SHK)
                           │
                          15kΩ
                           │
                          GND
```

| Resistor | Value | Ref (schematic) |
|----------|-------|-----------------|
| **10 kΩ** (SHK side) | **U6** **Line1_SHK** | **R19** |
| **15 kΩ** (GND side) | **U6** **Line1_SHK** | **R18** |
| **10 kΩ** (SHK side) | **U5** **Line2_SHK** | **R17** |
| **15 kΩ** (GND side) | **U5** **Line2_SHK** | **R16** |

**GPIO sits at the junction** between the two resistors (the tap).

At **SHK ≈ 4.5 V** high, the tap is about **4.5 × 15/(10+15) ≈ 2.7 V** — a valid
ESP32 input high and safely under **3.3 V**. At **SHK ≈ 5 V**, the tap is about **3.0 V**.
Schematic refs: **U6** **R19**/**R18**; **U5** **R17**/**R16** (see [designator map](#designator-map-schematic)).

#### Can firmware detect an unpopulated AG1171? Answered: no, not reliably

**Resolved (2026-07): no.** We tried reading Line 2's SHK/RM/FR/PD as plain
GPIO inputs at boot, before anything else claims them, to distinguish "AG1171
populated" from "microSD populated instead" (same GPIO 18-21 pads, XOR
stuffed). Real boards showed the readings **aren't reliable enough to use**:

| Board state | SCLK | MOSI | MISO | CS |
|---|---|---|---|---|
| Both SLICs populated, on-hook (boot A) | 0 | 0 | 0 | 1 |
| Both SLICs populated, on-hook (boot B, same board) | 0 | 1 | 1 | 1 |
| microSD populated (0R removed, SD board) | 0 | 1 | 1 | 1 |

A stuffed-SLIC board and an SD-card board produced the **same** reading, and
the SLIC board itself didn't read consistently boot-to-boot — these pins
mostly float without a strong driver, so a floating-pin snapshot can't be
trusted as a presence signal. Conclusion: firmware doesn't try to guess this
anymore. Which hardware is populated is a **build-time choice**
(`CONFIG_BOARD_MICROSD` in `idf.py menuconfig`, see
[`GETTING_STARTED.md`](../GETTING_STARTED.md)) that you set to match your
actual assembly. For microSD specifically, the definitive "is a card really
there" check is whether it mounts (`sd_test` on the console).

#### ASCII — AG1171 line interface (each SLIC)

```text
5V (VBUS branch) ──► +VPWR (AG1171)     470µF + 100nF to GND at module
GND ───────────────► GND

ESP32 GPIO ── PD net ──►|──► PD pin     (1N4148: cathode toward PD pin)
                         │
                    (anode to GPIO)

ESP32 GPIO ◄── SHK ── 10k ──┬── (from SIL SHK pin)
                            15k
                             │
                            GND

ESP32 GPIO ◄────────────  F/R   (output — ring cadence)
ESP32 GPIO ◄────────────  RM    (output — ringing mode)

VIN ── 10nF ── GND ──► ES8388 LIN1 or LIN2 (line input)
VOUT ◄── 100nF ── GND ◄── ES8388 LOUT1 or ROUT1 (line output)

Tip/Ring ◄──► RJ12 jack
              │
              └── [M7 ×4 bridge] per AG1171 Fig. 4 (D2–5)
```

**PD polarity (AG1171 + diode):** GPIO **low** = power-down; **high-Z** = run.
Verify `BOARD_PD_LEVEL_*` in firmware when porting from AG1170 THT board.

---



| Ref | Build A | Build B |
|-----|---------|---------|
| **J6** | Line 1 | Line 1 |
| **J7** | Line 2 | **DNP** |

Footprint: `Connector_RJ:RJ12_Amphenol_54601-x06_Horizontal` (through-hole).

---


### microSD — Card1 (build B)

Socket choice is **one land pattern per PCB revision** — GPIO routing is the same; footprint differs.

| Socket | Part | JLC/LCSC | Unit price (LCSC 1+) | Extended fee | Total ≈ (1 board) |
|--------|------|----------|----------------------|--------------|-------------------|
| **Default — push-push** | **HRO TF-01A** | **C91145** | **~$0.19** | **+$3** | **~$3.19** |
| **Alt — friction / push-pull** | **Molex 104031-0811** | **C585350** | **~$0.67** | **+$3** | **~$3.67** |
| **Alt — push-push (Molex)** | **503398-1892** | **C428492** | **~$0.99** | **+$3** | **~$3.99** |

Part cost is small — you mainly pay the **$3** JLC Extended loading fee once per unique LCSC number.

#### Default socket — HRO **TF-01A** (push-push)

| Item | Detail |
|------|--------|
| **Type** | microSD, **push-push** (insert / eject), right-angle SMD |
| **JLC/LCSC** | **C91145** — **~$0.19** each (1+) |
| **KiCad footprint** | `JLCImport:TF-01A` — import once: `easyeda2kicad --full --lcsc_id=C91145` or JLCImport / LCSC Manager plugin |
| **Symbol** | **`JLCImport:TF-01A`** (project `JLCImport.kicad_sym`; restart KiCad after first import) |
| **Logic** | **3.3 V** — no level shifters |

**Push-push:** insert card until click; push again to eject — good for a front-panel slot.
Place at **PCB edge** with enclosure cutout. Generic “TF” footprint used on many ESP32 boards.

#### Alternate — Molex **104031-0811** (friction fit)

| Item | Detail |
|------|--------|
| **Type** | microSD, **push-pull / friction fit**, right-angle SMD |
| **JLC/LCSC** | **C585350** — **~$0.67** each (1+) |
| **KiCad footprint** | `Connector_Card:microSD_HC_Molex_104031-0811` (stock) |
| **Use when** | You want stock KiCad footprint without import; pull card out to remove |

#### Alternate — Molex **503398-1892** (push-push)

| Item | Detail |
|------|--------|
| **Type** | **Push-push** (Molex brand) |
| **JLC/LCSC** | **C428492** — **~$0.99** each (1+) |
| **Footprint** | Import from EasyEDA / SnapEDA — **different** land pattern than **TF-01A** |

#### SPI wiring (SDSPI) — **Card1** pin hookup (**TF-01A**)

**Card1** uses symbol **`JLCImport:TF-01A`** / footprint **`JLCImport:TF-01A`**. Pin numbers
match the JLC import (standard microSD SPI order). Wire every pad — do not leave **VDD**,
**VSS**, or frame **GND** floating.

**Power (your question):** **VSS (pin 6) → GND** directly. **VDD (pin 4) → +3.3 V** with
**100 nF** bypass (**C4** or **C5** on **+3.3 V** near the socket) from **VDD** to **GND** — the cap is on the **supply** pin,
not on **VSS**. Same **+3.3 V** rail feeds the **10 kΩ** pull-ups on DAT2/DAT1.

| Card1 pin | TF-01A symbol name | Connect to | Notes |
|--------|-------------------|------------|-------|
| **1** | **DAT2(RSV)** | **3.3 V** via **R2** (**10 kΩ**) | SPI mode: unused by ESP32; keep **high** per [ESP-IDF SD pull-ups](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sd_pullup_requirements.html) |
| **2** | **CDDAT3(CS)** | **GPIO18** (`SD_CS`) | Chip select — ESP32 drives |
| **3** | **CMD(DI)** | **GPIO20** (`SD_MOSI`) | |
| **4** | **VDD** | **+3.3 V** | Socket supply; decouple with **C4**/**C5** (**100 nF**) to **GND** (see below) |
| **5** | **CLK(SCLK)** | **GPIO19** (`SD_CLK`) | |
| **6** | **VSS** | **GND** | Card **return** — tie to ground plane (not through the cap) |
| **7** | **DAT0(D0)** | **GPIO21** (`SD_MISO`) | |
| **8** | **DAT1(RSV)** | **3.3 V** via **R3** (**10 kΩ**) | SPI mode: unused; pull-up recommended |
| **9** | **CD** | **NC** (default) or spare **GPIO** input | Card-detect switch. **NC** is fine — firmware can detect mount failures. Optional: GPIO + **10 kΩ** to 3.3 V; switch usually pulls **low** when card is inserted (verify on [TF-01A datasheet](https://www.lcsc.com/datasheet/lcsc_datasheet_1811082127_Korean-Hroparts-Elec-TF-01A_C91145.pdf)) |
| **10** | **GND** | **GND** | Frame / shell pad (left) |
| **11** | **GND** | **GND** | Frame / shell pad (left rear) |
| **12** | **GND** | **GND** | Frame / shell pad (right rear) |
| **13** | **GND** | **GND** | Frame / shell pad (right) |

**Build B passives at Card1** (populate only on microSD build):

| Ref | Part | Value / connection | Package |
|-----|------|-------------------|---------|
| **C4** or **C5** | Cap | **100 nF**: **VDD (pin 4)** ↔ **GND** (shared **+3.3 V** bypass) | **0402** |
| **R2** | Resistor | **10 kΩ**: **DAT2 (pin 1)** → **+3.3 V** | **0402** |
| **R3** | Resistor | **10 kΩ**: **DAT1 (pin 8)** → **+3.3 V** | **0402** |

```text
+3.3V ──┬── VDD (pin 4)
        ├── 10k (R2) ── DAT2(RSV)  (pin 1)
        └── 10k (R3) ── DAT1(RSV)  (pin 8)

VDD (pin 4) ── 100nF (C4 or C5 on +3.3V near socket) ── GND

GND ────┬── VSS (pin 6)
        ├── GND (pins 10, 11, 12, 13)   [all frame pads]

GPIO18 ──────── CDDAT3(CS)  (pin 2)
GPIO19 ──────── CLK(SCLK)   (pin 5)
GPIO20 ──────── CMD(DI)     (pin 3)
GPIO21 ──────── DAT0(D0)    (pin 7)

CD (pin 9) ── NC  (or ──► spare GPIO + 10k pull-up to 3.3V)
```

| TF-01A pad | ESP32 / passives (build B) |
|------------|----------------------------|
| **VDD** (4) | **+3.3 V**; **100 nF** bypass (**C4**/**C5** class) → **GND** |
| **VSS** (6) | **GND** |
| **GND** (10–13) | **GND** (all four frame pads) |
| **CLK(SCLK)** (5) | GPIO19 |
| **CMD(DI)** (3) | GPIO20 (MOSI) |
| **DAT0(D0)** (7) | GPIO21 (MISO) |
| **CDDAT3(CS)** (2) | GPIO18 (CS) |
| **DAT2(RSV)** (1) | **R2** **10 kΩ** → **3.3 V** |
| **DAT1(RSV)** (8) | **R3** **10 kΩ** → **3.3 V** |
| **CD** (9) | **NC** or optional GPIO |

#### Storage split

| Media | Role |
|-------|------|
| Internal flash (SPIFFS) | Prompts, dial-plan clips |
| **microSD** (FAT) | **Recordings**, large archives |

Recording format (planned): **8 kHz mono** or **G.711** (~0.5–1 MB/min).

---


### Audio monitor (J5)

Optional **1×3** on **LOUT2** / **ROUT2** — debug **playback** toward the phones (does not
replace **LOUT1**/**ROUT1** → SLIC path).

| J5 pin | Signal |
|--------|--------|
| **1** | **L** ← **LOUT2** (line 1 / I²S left) |
| **2** | **R** ← **ROUT2** (line 2 / I²S right) |
| **3** | **GND** |

**Scope:** **100 nF** series per line. **Headphones:** **100–220 µF** + optional **47 kΩ–1 kΩ** bleed to **GND**.

Enable **LOUT2** / **ROUT2** in ES8388 register 4 in firmware.

---

### Status LED (GPIO11)

**Active-high** — ESP32 **sources** current when the LED is on.

```text
GPIO11 ──► R6 220Ω ──► D2 anode (+) ──► D2 cathode (−) ──► GND
```

| Ref | Part | Notes |
|-----|------|-------|
| **D2** | Red **1206** LED | Any JLC red 1206; confirm **anode** pad on footprint |
| **R6** | **220 Ω** **0402** | ~5 mA at **Vf ≈ 2.0 V** — readable, easy on the pin |

At **220 Ω**: \(I \approx (3.3 - V_f) / 220 \approx 5\text{–}6\) mA for a typical red **Vf**.

Firmware: drive **GPIO11** high to light the LED.

---


### Boot strap (GPIO9) — **J2**

**2.54 mm** **1×2** pin header (**J2**, not a tact switch). **No RESET header** on this build.

| Pin | Net |
|-----|-----|
| **1** | **GPIO9** (module pin 15) |
| **2** | **GND** |

**Jumper installed** = **GPIO9** shorted to **GND** → strap for USB/UART download at reset.
**Jumper removed** = normal boot (weak pull-up inside module).

To enter download mode: install jumper, toggle **USB** or power-cycle, then flash. Remove jumper for normal run.

KiCad: `Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical` (or SMD 1×2). Jumper cap: standard **2.54 mm** shunt.

---


### JLC vs Digikey sourcing

JLC **Basic** parts = no **$3/reel** Extended loading fee. **ICs, connectors, odd-voltage
LDOs, and ESD arrays are almost always Extended** — budget **~$3 per unique LCSC
part number**, not per board.

**Strategy for this project**

| Source | What |
|--------|------|
| **JLC economic (Basic)** | All **0402/0603/0805** R/C, **1N4148W**, **M7** bridge (**C95872**) ×4/SLIC, **D1** SS14 (**C2480**) |
| **JLC economic (Extended — pay once)** | **USB-C**, **U4 ES8388** **C365736** |
| **JLC optional Extended** | **Card1** microSD (build B) — or hot-air at home |
| **Hand-solder (Digikey)** | **U1** AP2112K, **U3** USBLC6 (Extended **SOT-23**), **U2** WROOM, **AG1171** (~**$9**), **RJ12**, **470 µF**, **BZT03C82** or **P6KE82** |

In KiCad BOM, set **DNP** on hand-solder refs for the JLC order (or use a separate
`bom-jlc.csv` without those lines).

**DigiKey hand-solder BOM:** [`hardware/kicad/Rotary_phone_SMD2/bom-digikey-hand-solder.csv`](kicad/Rotary_phone_SMD2/bom-digikey-hand-solder.csv) — upload at [DigiKey myLists](https://www.digikey.com/en/mylists/) → *Upload from spreadsheet*. Map **Quantity**, **DigiKey Part Number** (or **Manufacturer Part Number**), and **Customer Reference**. Rows with blank DigiKey # (**AG1171**, optional **ES8388**) must be ordered elsewhere. Delete build-B DNP rows (U6, J7, line-2 passives) or build-A-only rows depending on your stuffing option.

**Release BOM (SMD):** [see the BOM section below](#bom--smd-board-rotary_phone_smd2) · **Index:** [`hardware/BOM.md`](BOM.md)

#### Master BOM — JLC vs hand

| Ref | Function | JLC (LCSC) | JLC tier | Hand-solder (Digikey) | Notes |
|-----|----------|------------|----------|------------------------|-------|
| **U2** | ESP32-C6-WROOM-1-N16 | — | — | **ESP32-C6-WROOM-1-N16** ([Espressif](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-WROOM-1-N16/) or Mouser) | Castellated; hot air |
| **U4** | **ES8388** codec | **ES8388** **C365736** | Extended | **ES8388** (Everest) e.g. Digikey/LCSC | QFN-28 4×4; **JLC place** or hot-air |
| **U1** | 3.3 V LDO | **AP2112K-3.3** **C82942** | Extended (**DNP**) | [AP2112K-3.3TRG1CT-ND](https://www.digikey.com/en/products/detail/diodes-incorporated/AP2112K-3-3TRG1/4470746) | SOT-23-5; alt **ME6211C33M5G-N** |
| **U_LDO18** | 1.8 V LDO | — | — | **DNP** (WM8731 only) | **XC6206P182MR** — not stuffed with ES8388 |
| **USB1** | USB-C receptacle | **TYPE-C 16PIN 2MD(073)** **C2765186** | Extended | — | **JLC only** — see [Power](#power--usb-c--33-v-regulator-u1-usb1-u3) |
| **U3** | USB ESD | **USBLC6-2SC6** **C7519** | Extended (**DNP**) | [USBLC6-2SC6CT-ND](https://www.digikey.com/en/products/detail/umw/USBLC6-2SC6/16705896) | SOT-23-6 — hand-solder |
| **D1** | VBUS backflow block | **SS14** **C2480** | Basic | **SS14-FDICT-ND** | SMB; **JLC place** (Basic, no Extended fee) |
| **J2** | Boot strap | **1×2** 2.54 mm header + jumper | Basic (header) | 2.54 mm shunt | Pin1 **GPIO9**, pin2 **GND** |
| **U5 / U6** | AG1171 SLIC | — | — | Silvertel **AG1171** (~**$9**) | SIL; 1× or 2× per build |
| **J6 / J7** | RJ12 | — | — | Amphenol **54601-006** (6P6C) | THT |
| **Card1** | microSD (build B) | **TF-01A** **C91145** (~**$0.19**) | Extended (+**$3**) | Import footprint via JLC/EasyEDA | Push-push default; **DNP** build A |
| **D3** / **D4** | SLIC PD | **1N4148W** **C81598** | Basic | **1N4148W** **78-1N4148W-T-ND** | **U5** / **U6** PD |
| **D13** / **D14** | Line TVS | — | — | **BZT03C82-TAP** **BZT03C82-TAP-ND**; alt **P6KE82A-G** | DO-41 axial at SIL; Silvertel either/or |
| **D5–D8** / **D9–D12** | Tip/ring bridge | **M7** (1N4007) **C95872** ×4/SLIC | **Basic** | **M7** or **1N4007** | **SMA** — 4-diode bridge per SLIC |
| **C20** / **C21** | 470 µF / SLIC | — | — | **Rubycon 16YXJ470M8X11.5** [16YXJ470M8X11.5CT-ND](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) | THT at SIL |
| **R, C** | Passives | Generic **0402/0603/0805** | **Basic** | — | Use JLC Basic filter when assigning |

#### USB-C (JLC placement)

**There is no JLC Basic USB-C.** **USB1** is always JLC Extended (~**$3** + part). Pinout and
**EH → GND** are in [Power](#power--usb-c--33-v-regulator-u1-usb1-u3). **CC** resistors stay JLC Basic **0402**.

#### Hand-solder notes

- **U1** AP2112K and **U3** USBLC6: mark **DNP** in JLC BOM — Extended **SOT-23**, order on DigiKey BOM.
- **D1** SS14: **include on JLC BOM** (**C2480**, Basic — no Extended fee).
- **No 1.8 V LDO** with ES8388.
- **M7 bridge:** **4× per SLIC** — JLC Basic (**C95872**); wire as standard full-wave bridge (replaces **MB4S**).
- **SS14:** between USB **VBUS** pad and **+5V** rail (cathode toward +5V).
- **Extended fee math:** 1× USB-C + 1× ES8388 + optional 1× microSD ≈ **$6–9** loading —
  still far cheaper than **WM8731** (~$12 + $3) on JLC.

---


### KiCad symbols & footprints (passives & support)

Default **0402** passives for JLC economic assembly (**0603** for 10 µF ceramics,
**0805** for 22 µF when added). Libraries are KiCad **10** stock symbols/footprints
unless noted. Add missing `.pretty` tables to `Rotary_phone_SMD/fp-lib-table`
(`Resistor_SMD`, `Capacitor_SMD`, `Diode_SMD`, `Crystal`, `Connector_USB`,
`Connector_Card`, `Button_Switch_SMD`, `Package_DFN_QFN` if needed).

**Schematic tip:** Prefer generic `Device:R` / `Device:C` (set **Value** on the
symbol) over value-specific library parts. Assign footprint once per ref class.

#### Resistors (0402)

| Role | Value | Symbol | Footprint | Notes |
|------|-------|--------|-----------|-------|
| EN pull-up | 10 kΩ | `Device:R` | `Resistor_SMD:R_0402_1005Metric` | **R1** — **U2** **EN** |
| I²C pull-ups | 4.7 kΩ ×2 | `Device:R` | `Resistor_SMD:R_0402_1005Metric` | **R11** / **R12** → 3.3 V |
| SHK divider (×1 or ×2) | **10 kΩ** + **15 kΩ** | `Device:R` | `Resistor_SMD:R_0402_1005Metric` | **U6**: **R19**/**R18**; **U5**: **R17**/**R16** |
| microSD DAT1/DAT2 pull-ups | 10 kΩ ×2 | `Device:R` | `Resistor_SMD:R_0402_1005Metric` | **R2** / **R3**: DAT2 pin 1 & DAT1 pin 8 → 3.3 V (build B) |
| USB-C CC | 5.1 kΩ ×2 | `Device:R` | `Resistor_SMD:R_0402_1005Metric` | CC1/CC2 → GND |
| XOR stuffing (optional) | 0 Ω ×4 | `Device:R` | `Resistor_SMD:R_0402_1005Metric` | Value `0` — line2 *or* SD branch |
| Crystal load (if used) | 12–22 pF ×2 | `Device:C` | `Capacitor_SMD:C_0402_1005Metric` | Only if **not** using ESP32 MCLK |

Compact schematic: `Device:R_Small` / `Device:C_Small` — same footprints.

#### Ceramic capacitors — placement size guide

| Value | Package | Where on this board |
|-------|---------|---------------------|
| **10 nF** | **0402** | AG1171 **VIN** coupling (**C16**/**C19**) |
| **100 nF** | **0402** | Default bypass — **C4**/**C5** WROOM 3V3, **ES8388** (**C9**–**C13**), **Card1**, SLIC **C22**/**C23**, **VOUT** (**C17**/**C18**) |
| **1 µF** | **0402** | **C1** WROOM **EN** RC; **ES8388** **C6**/**C7**/**C8** |
| **10 µF** | **0603** | **C2**/**C11** LDO in/out, optional **AVDD** / optional **5 V** at SLIC |
| **22 µF** | **0805** | **C3** **+3.3 V** bulk; needs 6.3–10 V rating |
| **470 µF** | **THT radial** | **C20**/**C21** AG1171 **+VPWR** only — **not** 0402/0603/0805 |

**Rule of thumb:** if it is **≤ 1 µF** ceramic on a signal or supply bypass → **0402**.
**10 µF** ceramics → **0603** (better JLC stock and ripple than 0402 at 10 µF).
**22 µF** on 3.3 V → **0805**. **Electrolytic bulk (470 µF)** → **through-hole** at the SIL.

| Role | Value | Symbol | Footprint | Notes |
|------|-------|--------|-----------|-------|
| Bypass / coupling (most) | 100 nF, 10 nF, 1 µF | `Device:C` | `Capacitor_SMD:C_0402_1005Metric` | WROOM, **ES8388**, microSD, SLIC |
| Mid bulk | 10 µF | `Device:C` | `Capacitor_SMD:C_0603_1608Metric` | LDO in/out, optional AVDD / 5 V at SLIC |
| 3.3 V bulk (WROOM) | 22 µF | `Device:C` | `Capacitor_SMD:C_0805_2012Metric` | 6.3–10 V when added to schematic |
| 5 V bulk (USB) | 10 µF | `Device:C` | `Capacitor_SMD:C_0603_1608Metric` | At connector; 6.3 V min rating |
| EN RC delay | 1 µF | `Device:C` | `Capacitor_SMD:C_0402_1005Metric` | On **EN** pin |

#### Electrolytic (bulk — not 0402)

| Role | Value | Symbol | Footprint | Notes |
|------|-------|--------|-----------|-------|
| AG1171 bulk | 470 µF 16 V ×1/SLIC | `Device:C_Polarized` | `Capacitor_THT:CP_Radial_D10.0mm_P5.00mm` | **Rubycon 16YXJ470M8X11.5** — hand-solder near SIL |
| Alt (SMD) | 470 µF 16 V | `Device:C_Polarized` | `Capacitor_SMD:CP_Elec_8x10.5` | Optional machine-placed bulk if not using THT at SIL |

#### Diodes

| Role | Part | Symbol | Footprint | Notes |
|------|------|--------|-----------|-------|
| SLIC PD (×1 or ×2) | **1N4148W** | `Diode:1N4148W` | `Diode_SMD:D_SOD-123` | JLC **C81598** Basic; cathode → AG1171 **PD** |
| Tip/ring bridge (×4/SLIC) | **M7** (1N4007) | `Device:D` | `Diode_SMD:D_SMA` | JLC **C95872** Basic — **not** 1N4148 |
| THT fallback | 1N4148 | `Diode:1N4148` | `Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal` | PD only — not bridge |

#### Regulators

| Role | Part | Symbol | Footprint | Notes |
|------|------|--------|-----------|-------|
| **3.3 V** main LDO | **AP2112K-3.3** (alt **ME6211C33M5G-N**) | `JLCImport:ME6211C33M5G-N` | `JLCImport:ME6211C33M5G-N` | Hand-solder; JLC **C82942** DNP |
| **1.8 V** codec | — | — | — | **DNP** with ES8388; see [legacy WM8731](#legacy-wm8731-reference) |

#### USB ESD (U3)

Dedicated **USB 2.0** ESD IC — **do not** replace with discrete **1N4148** / generic TVS.

| Option | Part | JLC | Notes |
|--------|------|-----|-------|
| **Recommended** | **USBLC6-2SC6** | **C7519** DNP | Hand-solder [USBLC6-2SC6CT-ND](https://www.digikey.com/en/products/detail/umw/USBLC6-2SC6/16705896) |
| Alt | TPD2EUSB30 | Extended | Also Extended — not cheaper |

**Why not discrete passives?** USBLC6 gives **~3.5 pF** matched D+/D− clamps plus **VBUS** protection.
Random **Basic** TVS parts (e.g. **P6KE***, **PSM712**) are wrong voltage class or too much capacitance for USB data.

**SS14** on VBUS (**C2480**, Basic) is only backflow blocking — **not** a substitute for USBLC6 on D+/D−.

Placement: between **USB1** and ESP32 **GPIO12/13** (short traces).

#### USB & connectors

| Role | Part | Symbol | Footprint | JLC / Digikey |
|------|------|--------|-----------|---------------|
| USB-C receptacle | SHOU HAN **TYPE-C 16PIN 2MD(073)** | `Connector:USB_C_Receptacle` | Match **C2765186** | **JLC C2765186** (Extended) |
| USB ESD | **USBLC6-2SC6** | `Power_Protection:USBLC6-2SC6` | `Package_TO_SOT_SMD:SOT-23-6` | **C7519** Extended or hand Digikey |
| VBUS diode | **SS14** | `Device:D_Schottky` | `Diode_SMD:D_SMB` | **C2480** Basic — **JLC place** |

#### BOOT strap & LED

| Role | Symbol | Footprint | Sourcing |
|------|--------|-----------|----------|
| **J2** | `Connector:Conn_01x02` | `PinHeader_1x02_P2.54mm_Vertical` | JLC Basic 1×2 header + 2.54 mm jumper |
| Status LED | `Device:LED` | `LED_SMD:LED_1206_3212Metric` | JLC red 1206 + **220 Ω** **0402** |
| RESET | — | — | **DNP** — no header; power-cycle / USB |

#### Codec MCLK (ES8388)

MCLK comes from **ESP32 GPIO0** — **no crystal** on the codec. DNP crystal + load caps on U4.

| Role | Value | Symbol | Footprint | Notes |
|------|-------|--------|-----------|-------|
| Codec MCLK | — | — | — | ESP32 I²S master drives pin **1** at **12.288 MHz** for 48 kHz |

#### ICs & connectors (non-passive, for completeness)

| Role | Symbol | Footprint |
|------|--------|-----------|
| ESP32 module | `Scott:ESP32-C6-WROOM-1` (embedded in schematic) | `Scott:ESP32-C6-WROOM-1` |
| **ES8388** codec | `Audio:ES8388` | `Package_DFN_QFN:QFN-28-1EP_4x4mm_P0.45mm_EP2.6x2.6mm` |
| AG1171 SLIC | `Scott:AG1171_SIL` | `Scott:AG1171_SIL` (add to project lib) |
| microSD | **`JLCImport:TF-01A`** | **`JLCImport:TF-01A`** **C91145** (default); alt Molex **104031** footprint |
| RJ12 | `Connector:6P6C` or project jack sym | `Connector_RJ:RJ12_Amphenol_54601-x06_Horizontal` |

#### fp-lib-table additions (SMD project)

Ensure these KiCad stock libraries are on the footprint search path:

```text
Resistor_SMD, Capacitor_SMD, Capacitor_THT, Diode_SMD, Diode_THT,
Crystal, Connector_USB, Connector_Card, Button_Switch_SMD,
Package_TO_SOT_SMD
```

**ESP32 footprint:** `PCM_Espressif` is **not** in stock KiCad — it comes from Espressif’s
PCM add-on. This project vendors the official footprint at
`hardware/kicad/Rotary_phone_SMD2/Scott.pretty/ESP32-C6-WROOM-1.kicad_mod` so you do
not need a separate install. Optional: install
[espressif-kicad-addon.zip](https://github.com/espressif/kicad-libraries/releases/latest/download/espressif-kicad-addon.zip)
via KiCad **Plugin and Content Manager → Install from file** for 3D models and other modules.

---


### Assembly strategy (JLCPCB)

**KiCad update:** Schematic + PCB footprints use **0402** R/C, **0603** for 10 µF,
**0805** for 22 µF. See [JLC vs Digikey sourcing](#jlc-vs-digikey-sourcing).

| JLC economic (Basic) | JLC Extended (~$3 each) | Hand-solder after boards arrive |
|----------------------|-------------------------|----------------------------------|
| **0402/0603/0805** R/C | **USB1** USB-C **C2765186** | **U2** WROOM-1 |
| **1N4148W**, **M7** ×4/SLIC, **D1** SS14 (**C2480**) | **U4** ES8388 **C365736** | **U1** AP2112K, **U3** USBLC6 |
| | **Card1** microSD (build B) | **AG1171** (~**$9**), **J6/J7** RJ12, **470 µF**, **BZT03C82** or **P6KE82** |

**Typical Extended loading cost:** 1× USB-C + 1× ES8388 = **$6**; add **$3** more if JLC places **Card1** microSD.

Export BOM with **DNP** on all hand-solder refs for the JLC order. **WROOM-1** is always hand-solder;
**ES8388** is cheap enough to let JLC place.

---


### Firmware

| Setting | Dual-line (A) | Single + SD (B) |
|---------|---------------|-----------------|
| Pin header | `board_pins_smd.h` | same |
| Codec driver | **ES8388** (planned) | same |
| `CONFIG_BOARD_NUM_LINES` | `2` | `1` |
| `CONFIG_BOARD_MICROSD` | `0` | `1` |
| `BOARD_NUM_LINES` | 2 | 1 |

Dual-line THT firmware uses `board_pins.h` and the DevKit pin map — different
binary / `sdkconfig` from the SMD board.

PD polarity on AG1171 (diode) may differ from AG1170 (transistor) on the THT
board — verify `BOARD_PD_LEVEL_RUN` / `BOARD_PD_LEVEL_POWERDOWN` when porting.

---


### Schematic / layout checklist

- [ ] **U4** = **ES8388** (not WM8731); **DNP U_LDO18**; passives per [ES8388 BOM](#suggested-bom--es8388-passives--helper-circuits)
- [ ] **J2** 2.54 mm boot header (no RESET header); status LED **D2** + **R6** (**220 Ω**)
- [ ] **M7** ×4 bridge per SLIC (**C95872**), not MB4S / not 1N4148 on tip/ring
- [ ] **U1** **EN/CE** (pin 3) tied to **VIN** (pin 1) — not floating
- [ ] Replace DevKitM-1 with **WROOM-1** + Fig. 9-1 support (LDO, WROOM EN RC, USB-C)
- [ ] **Not** on PCB: 40 MHz crystal, flash, RF inductors (inside module)
- [ ] AG1170 → **AG1171**; PD diodes; drop +VA / BC846 networks
- [ ] SMD **0402** passives per [KiCad table](#kicad-symbols--footprints-passives--support); AG1171 support per datasheet Figure 4
- [ ] **Card1** = **HRO TF-01A C91145** (import footprint); DNP on build A
- [ ] Star-route GPIO **18–21** to U6 pads **and** **Card1** footprint
- [ ] Antenna keepout if using **WROOM-1** PCB antenna
- [ ] `Connector_Card` in `fp-lib-table`

#### Layout TODO (next spin)

- [ ] Change the **0 Ω** resistors (**R7–R10**) to **jumpers** if there is room (same XOR routing function, easier rework).
- [ ] Check **tip/ring** clearances are **≥ 0.8 mm** (phone line can see **~100 V** peak — `Rotary_phone_SMD2.kicad_dru`, **`PhoneLine`** net class).
- [ ] Use **1.0 mm** tip/ring traces where possible (RJ12 → bridge → TVS → AG1171).
- [ ] Add a **RESET** header/button (EN → GND, momentary), same style as **J2**. First SMD2 bring-up had no way to power-cycle the MCU without unplugging USB — with native USB and no UART bridge chip there's no DTR/RTS auto-reset circuit either, so **J2** (boot strap) is useless without a way to toggle EN alongside it.
- [ ] Add **test points on D+/D-** at the **U2** side of **U3** (post-ESD-chip) — hard to probe 0402/QFN pads directly with a scope when native USB won't enumerate and you need to check for PHY activity.
- [ ] Add a **GND test point** near **USB1** — USB signaling needs a solid common ground reference with the host; the WROOM-1's thermal/ground pad is under the module and its solder quality isn't inspectable or probable after reflow. A nearby GND test point at least lets you confirm continuity from the connector shield to system ground.
- [ ] Consider consolidating **J2/J3/J4** (boot strap, U0TXD, U0RXD) into one header alongside a GND pin, laid out to match a standard 3.3 V FTDI/USB-UART adapter pinout — makes it a single plug-in instead of three separate 2-pin headers when native USB debugging is a dead end (see bring-up notes below).

---


### Quick reference — build B (single RJ12 + microSD)

```text
J6 (RJ12) ── U5 ── ES8388 LIN2/ROUT1 ── I²S/I²C ── U2 (WROOM-1)
                              │
Card1 (TF-01A) ── SPI ────────┴── GPIO 18–21 (R2/R3 pull-ups)
USB1 ── GPIO 12/13
```

**Note:** Silk **“Line 1”** (**J6** / **U5**) is codec **right** channel; firmware **line 0** GPIOs (**1–3**, **10**) wire to **U6**/**J7**, which are **DNP** on build B — confirm stuffing matches your `sdkconfig` before fab.

---


### Legacy WM8731 reference

**Superseded** by **ES8388** on this board (~**$1 + $3** JLC vs ~**$12 + $3** for WM8731).
Kept for THT gateway comparison and if you ever revert the schematic.

Full pin guide: [`WM8731.md`](through-hole-wm8731/WM8731.md)

#### WM8731 vs ES8388 (quick compare)

| | WM8731 (old plan) | ES8388 (current) |
|---|-------------------|------------------|
| **Package** | SSOP-28 (hand-solder friendly) | QFN-28 4×4 (JLC place) |
| **Supply** | 3.3 V + **1.8 V DCVDD** | **3.3 V only** (1.8–3.3 V rated) |
| **I²C addr** | **0x1A** | **0x10** (CE → GND) |
| **I²S pins** | BCLK, DACDAT, DACLRC/ADCLRC, ADCDAT, MCLK | SCLK, DSDIN, LRCK, ASDOUT, MCLK |
| **Line 1 in/out** | LLINEIN / LOUT | LIN1+RIN1 / LOUT1 |
| **Line 2 in/out** | RLINEIN / ROUT | LIN2+RIN2 / ROUT1 |
| **Firmware** | `wm8731.c` (existing) | **New ES8388 driver** |

#### Suggested BOM — WM8731 decoupling (reference only)

| Ref | Part | Value / notes | Package |
|-----|------|----------------|---------|
| U_LDO18 | LDO 1.8 V | **XC6206P182MR** — Digikey **893-1276-1-ND** | SOT-23-3 |
| C_18V_IN / OUT | Cap | **1 µF** on LDO in and **DCVDD** | **0402** |
| C_AVDD / DBVDD / HPVDD | Cap | **0.1 µF** each rail | **0402** |
| C_AVDD_BULK | Cap | **10 µF** on **AVDD** | **0603** |

```text
3.3V ──► AVDD, DBVDD, HPVDD, MODE, CSB (strap GND → I²C 0x1A)
1.8V ──► DCVDD (pin 27)          ← XC6206P182MR from 3.3V

GPIO0  ── MCLK  ── pin 25
GPIO4  ── BCLK  ── pin 3
GPIO5  ── LRCK  ── pins 5 & 7 (tied)
GPIO6  ── DOUT  ── pin 4  (DACDAT)
GPIO7  ── DIN   ── pin 6  (ADCDAT)
GPIO22 ── SDA   ── pin 23
GPIO23 ── SCL   ── pin 24
```



---

## BOM — SMD board (`Rotary_phone_SMD2`)

**KiCad:** [`hardware/kicad/Rotary_phone_SMD2/`](../../hardware/kicad/Rotary_phone_SMD2/) · **Design:** [see the design reference above](#rotary-phone-gateway--smd-board) · **DigiKey CSV/cart export:** not committed to the repo (see note above about the missing 2026-07-02 export)

Most builders order **JLCPCB** for SMT passives, then fill in the hand-solder table below. **DigiKey** links/prices match [`2026-07-02T230626.csv`](2026-07-02T230626.csv) where listed.

Mark rows done in the leading empty column (e.g. `x` or `☑`), or strike through the component name.

---

### Build options

| Build | Lines | microSD | Omit from hand-solder table |
|-------|-------|---------|----------------------------|
| **A — Dual line** | **J6** + **J7** | No | **Card1** |
| **B — Single + SD** | **J6** only | **Card1** | **U6**, **J7**, **C21**, **D4**, **D14**, **R16–R19** (line-2 bridge **D9–D12** are JLC only) |

Firmware: `CONFIG_BOARD_NUM_LINES` 2 (A) or 1 (B); `CONFIG_BOARD_MICROSD` 0 (A) or 1 (B).

---

### Hand-solder BOM

#### Modules & ICs

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | Microcontroller: **U2** ESP32-C6-WROOM-1-N16 | [ESP32-C6-WROOM-1-N16-ND (~$5)](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-WROOM-1-N16/14569322) | | Castellated; hot-air / reflow |
| | 1 | Audio codec: **U4** ES8388 (QFN-28) | [C365736 / LCSC (~$1–3)](https://www.lcsc.com/product-detail/C365736.html) | | Usually JLC-placed; hand if DNP |
| | 1 | SLIC: **U5** AG1171 @ **J6** | [5061-AG1171-ND ($8.50)](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) | | Line 1 |
| | 1 | SLIC: **U6** AG1171 @ **J7** | same as **U5** | | Build **A** only; line 2 |
| | 1 | microSD socket: **Card1** TF-01A | [C91145 / LCSC (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | | Build **B** only; usually JLC-placed |

#### Regulators & protection

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | Regulator (3.3 V LDO): **U1** AP2112K-3.3 (SOT-23-5) | [AP2112K-3.3TRG1DICT-ND ($0.17)](https://www.digikey.com/en/products/detail/diodes-incorporated/AP2112K-3-3TRG1/4470746) | | JLC DNP; alt ME6211C33M5G-N |
| | 1 | USB ESD protection: **U3** USBLC6-2SC6 | [USBLC6-2SC6CT-ND (~$0.60)](https://www.digikey.com/en/products/detail/umw/USBLC6-2SC6/16705896) | | JLC DNP |
| | 1 | Line TVS: **D13** BZT03C82 or P6KE82 | [BZT03C82-TAP-ND (~$1)](https://www.digikey.com/en/products/detail/vishay-general-semiconductor-diodes-division/BZT03C82-TAP/7464977) | | @ **J6** / **U5**; DO-41 axial; alt **P6KE82** per Silvertel |
| | 1 | Line TVS: **D14** BZT03C82 or P6KE82 | same as **D13** | | Build **A**; @ **J7** |

#### Connectors & headers

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **J6** RJ12 horizontal | [609-1055-ND ($0.83)](https://www.digikey.com/en/products/detail/amphenol-icc/87180-062LF/1159085) **87180-062LF** | | Alt **54601-006** [609-54601-006R-ND](https://www.digikey.com/en/products/detail/amphenol-icc/54601-006/1158894) |
| | 1 | **J7** RJ12 horizontal | same as **J6** | | Build **A** only |
| | 1 | **J2** 1×2 header 2.54 mm | [S1011EC-40-ND (~$0.50)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-02-UA/5828900) | | Boot strap GPIO9 ↔ GND |
| | 1 | **J2** jumper 2.54 mm | [S9002-ND (~$0.10)](https://www.digikey.com/en/products/detail/sullins-connector/SH1-002-00/614404) | | Download mode only |
| | 1 | **J5** 1×3 header (optional) | [S1012EC-40-ND (~$0.55)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-03-UA/5828902) | | Monitor L/R/GND |
| | 1 | **J3** 1×2 UART TX (optional) | [S1011EC-40-ND (~$0.50)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-02-UA/5828900) | | GPIO16 |
| | 1 | **J4** 1×2 UART RX (optional) | [S1011EC-40-ND (~$0.50)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-02-UA/5828900) | | GPIO17 |
| | 1 | **J8** 2-pin 5 mm terminal (optional) | [277-1247-ND (~$1)](https://www.digikey.com/en/products/detail/phoenix-contact/1935161/2771247) | | Bench +5 V |

#### Diodes

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **D3** 1N4148W (SOD-123) | [78-1N4148W-T-ND (~$0.10)](https://www.digikey.com/en/products/detail/stmicroelectronics/1N4148W/1014052) | | **U5** PD; usually JLC |
| | 1 | **D4** 1N4148W | [78-1N4148W-T-ND (~$0.10)](https://www.digikey.com/en/products/detail/stmicroelectronics/1N4148W/1014052) | | Build **A**; **U6** PD |

#### Capacitors

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **C20** 470 µF 16 V radial | [1189-1146-ND ($0.24)](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) **16YXJ470M8X11.5** | | **U5** +VPWR bulk |
| | 1 | **C21** 470 µF 16 V radial | same as **C20** | | Build **A**; **U6** +VPWR |

Upload [`bom-digikey-hand-solder.csv`](kicad/Rotary_phone_SMD2/bom-digikey-hand-solder.csv) to [DigiKey myLists](https://www.digikey.com/en/mylists/) for bulk DigiKey ordering.

**Typical hand-solder kit (DigiKey, build A):** ~$20–25 per board (+ WROOM, USBLC6, TVS, headers — not in cart CSV), excluding shipping.

---

### DigiKey cart reference (2026-07-02)

From [`2026-07-02T230626.csv`](2026-07-02T230626.csv) — shared parts for SMD + through-hole builds:

| DigiKey PN | MPN | Unit | Used on |
|------------|-----|------|---------|
| 5061-AG1171-ND | AG1171 | $8.50 | SLIC |
| 609-1055-ND | 87180-062LF | $0.83 | RJ12 |
| 1189-1146-ND | 16YXJ470M8X11.5 | $0.24 | SMD **C20/C21** |
| AP2112K-3.3TRG1DICT-ND | AP2112K-3.3TRG1 | $0.17 | SMD **U1** |
| MCP1700T1802ETTCT-ND | MCP1700T-1802E/TT | $0.51 | SOT-23 — **not** THT **U3** (needs SOT-89) |
| 5399-BC846ACT-ND | BC846A | $0.05 | THT **Q1/Q2** |
| CF14JT4K70CT-ND | CF14JT4K70 | $0.02 | THT **R1/R2** |
| CF14JT15K0CT-ND | CF14JT15K0 | $0.02 | THT **R3/R5** |

---

### JLCPCB SMT assembly

Mark all hand-solder refs **DNP** on the JLC BOM. **Extended loading:** ~$3 per unique non-Basic LCSC (**USB1**, **U4**; build B add **Card1**).

JLC parts use **LCSC** links (not DigiKey). Typical total for 5 assembled boards: ~$35–58.

#### Modules & connectors

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **U4** ES8388 | [C365736 (~$1)](https://www.lcsc.com/product-detail/C365736.html) | Extended |
| 1 | **USB1** USB-C 16P | [C2765186 (~$0.25)](https://www.lcsc.com/product-detail/C2765186.html) | Extended |
| 1 | **Card1** TF-01A | [C91145 (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | Build B; Extended |
| 1 | **U1** AP2112K-3.3 | [C82942 (~$0.15)](https://www.lcsc.com/product-detail/C82942.html) | **DNP** — hand |
| 1 | **U3** USBLC6 | [C7519 (~$0.25)](https://www.lcsc.com/product-detail/C7519.html) | **DNP** — hand |

#### Diodes

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **D1** SS14 | [C2480 (~$0.05)](https://www.lcsc.com/product-detail/C2480.html) | Basic; VBUS → +5 V |
| 1 | **D2** LED 1206 red | [C2286 (~$0.01)](https://www.lcsc.com/product-detail/C2286.html) | Basic; GPIO11 |
| 1 | **D3** 1N4148W | [C81598 (~$0.01)](https://www.lcsc.com/product-detail/C81598.html) | Basic |
| 1 | **D4** 1N4148W | [C81598 (~$0.01)](https://www.lcsc.com/product-detail/C81598.html) | Build A |
| 4 | **D5–D8** M7 (1N4007) | [C18199088 (~$0.02)](https://www.lcsc.com/product-detail/C18199088.html) | Bridge @ **J6** |
| 4 | **D9–D12** M7 | [C18199088 (~$0.02)](https://www.lcsc.com/product-detail/C18199088.html) | Build A @ **J7** |

#### Resistors (0402)

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **R1** 10 kΩ | [C25744](https://www.lcsc.com/product-detail/C25744.html) | **U2** EN |
| 2 | **R2**, **R3** 10 kΩ | [C25744](https://www.lcsc.com/product-detail/C25744.html) | Build B; microSD |
| 2 | **R4**, **R5** 5.1 kΩ | [C25905](https://www.lcsc.com/product-detail/C25905.html) | USB CC |
| 1 | **R6** 220 Ω | [C25091](https://www.lcsc.com/product-detail/C25091.html) | LED |
| 4 | **R7–R10** 0 Ω | [C17168](https://www.lcsc.com/product-detail/C17168.html) | XOR line2 / SD |
| 2 | **R11**, **R12** 4.7 kΩ | [C25900](https://www.lcsc.com/product-detail/C25900.html) | I²C |
| 1 | **R13** 10 Ω | [C25077](https://www.lcsc.com/product-detail/C25077.html) | HPVDD |
| 2 | **R14**, **R15** 47 kΩ | [C25792](https://www.lcsc.com/product-detail/C25792.html) | Monitor bleed |
| 1 | **R16** 15 kΩ | [C25756](https://www.lcsc.com/product-detail/C25756.html) | Build A SHK |
| 1 | **R17** 10 kΩ | [C25744](https://www.lcsc.com/product-detail/C25744.html) | Build A SHK |
| 1 | **R18** 15 kΩ | [C25756](https://www.lcsc.com/product-detail/C25756.html) | Build A SHK |
| 1 | **R19** 10 kΩ | [C25744](https://www.lcsc.com/product-detail/C25744.html) | Build A SHK |

#### Capacitors

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **C1** 1 µF 0402 | [C52923](https://www.lcsc.com/product-detail/C52923.html) | EN |
| 1 | **C2** 10 µF 0603 | [C15525](https://www.lcsc.com/product-detail/C15525.html) | +5 V |
| 1 | **C3** 22 µF 0805 | [C59461](https://www.lcsc.com/product-detail/C59461.html) | +3.3 V |
| 2 | **C4**, **C5** 100 nF 0402 | [C1525](https://www.lcsc.com/product-detail/C1525.html) | Bypass |
| 3 | **C6–C8** 1 µF 0402 | [C52923](https://www.lcsc.com/product-detail/C52923.html) | **U4** |
| 5 | **C9–C13** 100 nF 0402 | [C1525](https://www.lcsc.com/product-detail/C1525.html) | **U4** |
| 1 | **C11** 10 µF 0603 | [C15525](https://www.lcsc.com/product-detail/C15525.html) | LDO |
| 2 | **C14**, **C15** 100 µF 1206 | [C15008](https://www.lcsc.com/product-detail/C15008.html) | Monitor |
| 2 | **C16**, **C19** 10 nF 0402 | [C15195](https://www.lcsc.com/product-detail/C15195.html) | SLIC |
| 2 | **C17**, **C18** 100 nF 0402 | [C1525](https://www.lcsc.com/product-detail/C1525.html) | SLIC |
| 2 | **C22**, **C23** 100 nF 0402 | [C1525](https://www.lcsc.com/product-detail/C1525.html) | +5 V @ SLIC |
| 2 | **C24**, **C25** 100 nF 0402 | [C1525](https://www.lcsc.com/product-detail/C1525.html) | Codec |

#### JLC order checklist

| | Step |
|---|------|
| | Generate Gerber + BOM + CPL (JLCPCB Tools) |
| | DNP all hand-solder refs on JLC BOM |
| | Extended: **C2765186**, **C365736**; build B add **C91145** |
| | Verify **D2** LED polarity in placement preview |

---

### Revision

| Date | Notes |
|------|-------|
| 2026-07 | DigiKey prices from `2026-07-02T230626.csv` |
| 2026-06 | Table format: Qty, Component, DigiKey, AliExpress, Notes |


---

## Signal-level rework history (DTMF clipping, AEC timing)

Moved here from the old `HARDWARE_SIGNAL_LEVELS.md` §1B/§1C -- the general
WM8731/through-hole signal-level reference that file also carried moved to
`through-hole-wm8731/SIGNAL_LEVELS.md` instead, since it's unrelated to this
board.

### 1B. ES8388 + AG1171 (SMD2/SMD3 production board) — measured DTMF clipping, 2026-08

**Symptom:** real DTMF dialing on a physical handset (MaxPhone, DualPhone —
both SMD2, confirmed identical circuit) intermittently failed to register
digits. Root-caused to hard ADC clipping, not a detector bug — captured
live via a temporary `peak` diagnostic in `dtmf_detector_push()`
(`firmware/components/dtmf/dtmf.c`): **every correctly-classified digit
showed `peak=32768`**, i.e. int16 full-scale, on both boards.

#### Datasheet numbers (`datasheets/ES8388.pdf` §8.3, "ADC Analog and Filter
Characteristics")

| Parameter | Value | Notes |
|---|---|---|
| Full Scale Input Level | **AVDD/3.3 Vrms** → **1.0 Vrms** at AVDD = 3.3 V (this board) | RMS of a single-tone test signal — see crest-factor caveat below |
| Input Impedance | **20 kΩ** | Divider sizing needs to account for this loading, not just the nominal ratio |

(A `pdftotext -layout` extraction of this PDF now exists alongside the
other `*_extracted.txt` files — see §6 below.)

#### Measured levels (oscilloscope, DualPhone, wire tapped on the AC side of
AG1171 VOUT — i.e. before the existing SLIC→codec coupling cap)

| Signal | Vmin | Vmax | Vpp | Vrms (scope reading) |
|---|---|---|---|---|
| Dial tone (locally generated, firmware `audio_tx_tone()`) | 4.3 V | 4.6 V | 0.3 V | 4.5 V |
| Real DTMF digit "5" (physical handset) | 3.3 V | 5.8 V | 2.5 V | 4.6 V |

Note: the scope's Vrms reading is dominated by the ~4.5 V DC bias the
signal rides on (AC-coupled at the codec end, but this tap point is
*before* that cap) — it's not directly comparable to the datasheet's
Vrms-of-AC-signal spec. Vpp is the more useful number here.

Dial tone (0.3 Vpp) does not clip. Real DTMF (2.5 Vpp) clips hard, despite
being — on a naive single-tone RMS estimate — not obviously over the 1.0
Vrms full-scale spec. The gap is DTMF's **crest factor**: it's two tones
summed, and a summed dual-tone signal's instantaneous peaks run well above
what a same-RMS single tone would produce (this is *why* telecom designs
conventionally spec extra headroom for DTMF specifically, not a fluke of
this board). The datasheet's RMS spec doesn't warn you about this by
itself; the scope measurement is what caught it.

#### Ruled out: codec gain

`micamp 0 0` (ES8388 ADC PGA at its floor, 0 dB — console command,
`firmware/components/bringup/bringup.c`) still clips identically on every
digit. This proves the problem is upstream of the codec's programmable
gain stage entirely — the raw analog signal already exceeds the ADC's
input range before the codec's own gain has any say. Not fixable in
software; needs an analog attenuator between AG1171 VOUT and the ES8388
input pin.

#### Recommended divider (final V3 values — see "Built and confirmed working" below)

Target: get the 2.5 Vpp DTMF signal down to roughly **1.0–1.2 Vpp** at the
codec pin (real margin below the ~2.8 Vpp single-tone-equivalent full
scale, to cover DTMF's crest factor).

**Initial estimate: 10 kΩ series (R1) + 7.5 kΩ shunt-to-ground (R2)** —
nominal ratio ≈0.43, giving ~1.07 Vpp from 2.5 Vpp input. The ES8388's 20
kΩ input impedance loads the shunt leg (7.5 kΩ ∥ 20 kΩ ≈ 5.45 kΩ),
pulling the *effective* ratio closer to ~0.35 in practice — the loading
interaction is why this starting estimate wasn't treated as exact, and
why it was bench-verified against actual resistors on hand rather than
built to spec sight-unseen (see "Built and confirmed working" below for
the actual final values, 10 kΩ / 8.2 kΩ, not 7.5 kΩ).

**Built and confirmed working (DualPhone, 2026-08-19):** 100 nF coupling
cap (unchanged value, relocated to leaded/flying-lead per the rework plan
below) + **10 kΩ series / 8.2 kΩ shunt** (8.2 kΩ picked over 6.8 kΩ for
slightly better voice SNR margin — both were safely under the clipping
ceiling on paper, see the resistor-choice discussion in this session's
history). `tonemon` with all 12 keys (`0-9`, `*`, `#`) dialed on a real
handset: every digit classified correctly, `peak=` topped out around
14,000-16,900 — comfortably under the 32768 ceiling, roughly half of full
scale. Twist ratios on the accepted blocks also tightened up dramatically
(clustered ~0.6-1.2, vs. the wild 0.4-400+ swings seen while clipping) —
a clean signal now, not just technically under the ceiling. Verified with
`micamp` back at its normal boot-time default (0x11, +3dB/+3dB — see the
re-tuning note below), so this margin holds under normal operating gain,
not just the artificially-reduced 0 dB used during diagnosis.

**Placement — after the coupling cap, not before:**

- The divider goes on the AC side of the existing SLIC→codec coupling
  cap, close to the codec, not directly across AG1171 VOUT. Putting it on
  the SLIC's raw output would load that output stage (SLIC output drivers
  are designed for a given minimum load impedance) and fight the DC bias
  the cap is there to strip.
- **Rework plan for an already-populated board** (cap footprint sits right
  at the codec, no room for a 3rd component in series): desolder the
  existing SMD coupling cap, put **R1 in its vacated pad**, and bridge a
  new leaded/flying-lead capacitor from AG1171 VOUT to that same pad's
  SLIC-side node — recovers the cap-then-resistor order without needing
  new PCB real estate.
- **R2 (shunt to ground) goes after R1, at the codec side** — not between
  the cap and R1. If R2 sits between the cap and R1 instead, it forms a
  frequency-dependent high-pass network with the cap's own impedance
  rather than a clean resistive divider, and the codec pin ends up
  depending on the ES8388's own 20 kΩ input impedance to set the ratio
  instead of a known value. With R2 after R1, the cap's impedance is
  negligible at audio frequencies, so R1/R2 alone set the ratio.
  Convenient side effect of the rework above: R1's relocated pad is
  already right next to the codec — exactly where R2 needs to land too.
- **Minor side effect to be aware of:** adding R1+R2 downstream of the
  coupling cap raises its high-pass corner slightly (rough estimate with a
  100 nF cap: ~80 Hz today, bare 20 kΩ codec load, → ~105 Hz with the
  divider's ~15-17 kΩ added). Still well below the 300 Hz+ phone band, so
  this shouldn't be audible — noted here in case a low-frequency response
  question ever comes up later.

**Verification method:** live digital readback via the `dtmf` component's
`peak` diagnostic (temporary — see git log for `firmware/components/dtmf/dtmf.c`)
watched through `tonemon` (bring-up console) while dialing real digits.
`micamp 0 0` first, to isolate codec-gain from the real problem before
building anything. Bench-test candidate resistor values with wire leads
before committing to a permanent rework, checking `peak=` drops below
32768 on every digit as the pass/fail signal.

#### Re-tuning after the hardware fix — mic PGA gain must stay at 0dB

`micamp 0 0` was originally used only as a diagnostic, to isolate codec-gain
from the upstream ADC-clipping problem before the divider existed. It turned
out to matter for real, for an unrelated reason discovered later: once the
divider was in place, normal speech on the divided line came through as
near-silence (`capture` RMS ~0–300, vs. line 2's unmodified ~3000+ baseline)
at the firmware's then-default +3dB/+3dB mic gain, while DTMF still
classified fine through the same path — a "tone dialing works, voice
doesn't" split that pointed at gain, not the resistor rework.

**Root cause:** the ES8388's mic PGA is (empirically observed, not spelled
out in the datasheet) a resistive-feedback gain stage whose *input*
impedance drops as its gain setting increases. The divider's ratio was
designed against the codec's datasheet-quoted static ~20 kΩ input
impedance (see the table at the top of this section) — that spec is
apparently only accurate at 0dB. At +3dB and above, the real input
impedance collapses far enough to swamp the 8.2 kΩ shunt, and the fixed
10 kΩ series resistor (R1) then drops most of the signal before it ever
reaches the ADC. DTMF still worked at +3dB purely because it's so much
louder than voice to begin with (that's the whole reason it was clipping
pre-divider) — enough survived the collapse to clear the detector's
thresholds; ordinary speech didn't have that margin.

**Fix:** `micamp` is a live I2C register poke, not persisted to NVS, so the
firmware's *boot default* is what actually matters. Changed the default in
`components/es8388/es8388.c` from +3dB/+3dB (`0x11`) to 0dB/0dB (`0x00`,
also the chip's own power-on-reset default). Confirmed on real hardware
(DualPhone, 2026-08-21): `capture` during loud talking went from flickering
between ~0 and a few hundred RMS at +3dB to a sustained, consistent
800–2100 RMS at 0dB — matching the divider's expected ratio applied to
line 2's unmodified baseline. Re-verified `tonemon` with all 12 keys at
0dB: every digit still classifies correctly, so this doesn't cost DTMF
anything.

The original +3dB default existed to break an unrelated acoustic/sidetone
feedback loop (both lines' mic+speaker live simultaneously) discovered
2026-07-20 — see the comment in `es8388.c` above the register write. 0dB is
strictly less gain than the +3dB that already killed that loop, so it
doesn't reopen it.

**MaxPhone:** now has the same 10 kΩ/8.2 kΩ rework applied (confirmed same
circuit as DualPhone). MaxPhone has a rotary handset (no keypad), so the
`tonemon` + `peak=` DTMF verification above doesn't apply there directly —
its line has only been exercised via pulse dialing so far, not confirmed
against a `peak=` reading the way DualPhone's DTMF path was.

---

### 1C. Local echo (AEC) path timing — measured, 2026-08-22

Sizing the software echo canceller (`firmware/components/aec/`) needs two
numbers per board: **bulk delay** (how far back to anchor the adaptive
filter) and **tail length** (how far past that it has to reach). Measured
with the bring-up `echoprobe [line] [# repeats]` command (10 ms, 1 kHz burst
out the earpiece, listens on the same line's own mic — see the command's own
comment in `bringup.c` for why this, not a live call, is what actually sizes
a canceller). Repeat mode runs N probes back-to-back and reports how many
found a real leak, since a single run is vulnerable to ambient room noise
inflating the "noise floor" baseline and hiding a real echo behind the
auto-detect threshold — every result below is from repeat mode (5 runs) or a
clean (low, single-digit noise floor) single run.

| Board | Handset position | Bulk delay | Tail | Total span | Consistency |
|---|---|---|---|---|---|
| DualPhone | Against ear (earpiece covered) | 23.2 ms | 12.5 ms | ~36 ms (285 taps) | 5/5 (1 discarded — noisy) |
| DualPhone | Face-down on desk | 23.2 ms | 12.5 ms | ~36 ms (285 taps) | 5/5 |
| DualPhone | Face-up on desk | 23.2 ms | 12.5 ms | ~36 ms (285 taps) | 5/5 — **but see note below** |
| MaxPhone | Against ear (earpiece covered) | 20.3 ms | 10.8 ms | ~31 ms (248 taps) | 4/5 (1 discarded — noisy) |
| MaxPhone | Face-down on desk | 21.3 ms | 10.1 ms | ~31 ms (246–248 taps) | 5/5 |
| MaxPhone | Face-up on desk | 20.3 ms | 10.6 ms | ~31 ms (246–248 taps) | 5/5 |

Bulk delay is very tightly consistent *within* a session (DualPhone read
23.2 ms in every clean run here regardless of position; MaxPhone's 20.3 vs.
21.3 ms across positions is likely the same kind of jitter, not a real
per-position difference), but drifted a few ms *between* sessions — an
earlier session measured DualPhone at 26.9–27.0 ms. Total span (delay +
tail) stays well inside the 40 ms filter either way, so this doesn't change
the conclusion below, but it's worth knowing the exact bulk-delay figure
isn't perfectly stable boot to boot (likely I2S/DMA buffer phase alignment
at startup, not measurement error — each session's own repeat-mode runs
agree with each other tightly).

**Conclusion: the current filter (320 taps = 40 ms) already comfortably
covers the real, repeatable local echo path on both boards, in every
position tested — no `AEC_TAPS` increase needed.** Both boards' bulk
delay + tail lands in the ~30–36 ms range regardless of handset position,
including face-down (the original suspected "worst case"). This directly
supersedes an earlier single, uncontrolled measurement that showed a ~380 ms
tail on DualPhone — with the earpiece open to the room and no repeat/noise
filtering, that number turned out to be a room-acoustics artifact of the
test setup, not a property of the board or handset (see below).

**Anomaly — intermittent long-tail room reflection, DualPhone face-up
only:** one single-run capture (handset face-up on the desk, otherwise
identical setup to the clean 5/5 run directly before it) showed a real,
structured return continuing to **~335 ms** (2,681 taps) — but the 5-run
repeat taken moments earlier, same position, untouched, showed a consistent
~36 ms span every time. Not reproduced on MaxPhone in any position tested.
Two things distinguish it from a real echo-cancellation requirement: (1) not
reproducible even holding the position fixed — comes and goes for reasons
not isolated (room airflow, minor settling, etc.), and (2) far too quiet to
matter even when present — its peak envelope RMS was ~12–58 across that
long tail, vs. ~700–2,000+ RMS for the real (~30 ms) echo on the same
board. Physically consistent with open-air reflection off a distant surface
(ceiling/wall) that only the face-up, unobstructed orientation can reach —
both "against ear" and "face-down" block that path, which is also why it
never shows up in the position the phone is actually used in. Not something
the AEC's fixed tap length needs to reach.

**AEC delay defaults (NVS-persisted per board, not a firmware constant —
see `board_aec_delay_get/set()` in `board.c`):** set from the table above —
DualPhone `aec delay 23 0`, MaxPhone `aec delay 20 0`. One firmware image
runs on both boards, but each has its own measured bulk delay, so this
can't be a single compiled default (`AEC_DEFAULT_DELAY_MS` in `aec.h`
remains the fallback for a board that's never had `aec delay`/`aec sweep`
run on it).

---

