# Power, logic, and audio levels (WM8731 + Silvertel SLIC + Pi Zero 2 W / ESP32)

This note is derived from the **local datasheet copies** in `datasheets/` (text extractions: `*_extracted.txt`). Numbers cite **Wolfson WM8731** production data **Rev 3.4, April 2004** and **Silvertel AG1170** full datasheet (local PDF; revision/date on cover sheet — see extract). Optional **AG1171** PDF remains in `datasheets/` as a **sibling-part** reference only.

**Covers both hardware variants:** §1 is the through-hole reference board (WM8731 + AG1170 + Pi Zero 2 W / ESP32). §1B is the SMD2/SMD3 production board (ES8388 + AG1171 + ESP32) — same signal-chain questions, different parts, added 2026-08 after a live clipping investigation on that board.

---

## 1. WM8731 (codec)

Pin-by-pin functional guide (KiCad **U1** symbol, gateway wiring): **`hardware/WM8731.md`**.

### Supplies (WM8731, not the “L” variant)

| Rail | Symbol | Min | Typ | Max | Notes |
|------|--------|-----|-----|-----|--------|
| Digital core | **DCVDD** | 1.42 V | 1.5 V | 3.6 V | Characterization often at **1.5 V** core with **3.3 V** I/O. |
| Digital **I/O buffer** | **DBVDD** | 2.7 V | **3.3 V** | 3.6 V | **I2S + I²C digital pins** referenced to this. |
| Analog | **AVDD**, **HPVDD** | 2.7 V | **3.3 V** | 3.6 V | Line in/out, headphone driver supply. |

**Constraint (datasheet note):** **DCVDD ≤ AVDD** always. **AGND** and **DGND** must stay within **0.3 V** of each other.

**Absolute maximum (digital/analog supply):** **+3.63 V** — treat **3.3 V nominal** as the design rail; do **not** run this codec from 5 V.

### Digital logic (I2S, MODE, CSB, SDIN, SCLK, XTI/MCLK)

With **DBVDD = 3.3 V**, thresholds are **CMOS vs DBVDD**:

- **V_IL** ≤ **0.3 × DBVDD** (~1.0 V)
- **V_IH** ≥ **0.7 × DBVDD** (~2.3 V)

**Pi Zero 2 W** and **ESP32** GPIO in push-pull **3.3 V CMOS** mode are a **good match** for the WM8731 when **DBVDD = 3.3 V**.

**Absolute maximum on digital inputs:** datasheet lists **DGND − 0.3 V** to **DVDD + 0.3 V** in the stress table — in practice **never exceed the pin’s active buffer supply (DBVDD)** on I2S/control pins, and respect **signal integrity** (no 5 V ever on these pins).

### Analog inputs: “mono mic” vs two phone lines (do you need two WM8731s?)

The WM8731 has **three named analog inputs**, but they are **not** “three simultaneous independent ADC channels” in the sense of three stereo pairs:

| Pin / path | Role |
|------------|------|
| **LLINEIN** | **Left line** input → **one** ADC channel (left). |
| **RLINEIN** | **Right line** input → **one** ADC channel (right). |
| **MICIN** | **Mono microphone** path into the ADC block, with **mux / routing** vs line inputs (see datasheet “ADC input multiplexer” — line vs mic is a **mode**, not an extra fourth concurrent wideband line pair). |

For **two RJ11 lines** you only need **stereo line I/O**:

- **DAC:** **LOUT** → AG1170 #1 **VIN**, **ROUT** → AG1170 #2 **VIN** (Pi → each phone line).
- **ADC:** AG1170 #1 **VOUT** → **LLINEIN**, AG1170 #2 **VOUT** → **RLINEIN** (each phone line → Pi).

That is **2× DAC + 2× ADC** at **telephone bandwidth**, which is exactly what the **stereo line** path provides. **MICIN can be left unconnected** (or used later for a separate handset mic — not required for the SLIC two-line gateway).

**Conclusion:** **One WM8731** is enough for a **two-line** box. You do **not** need a second WM8731 unless you want **more than two** analog line interfaces, redundant codecs, or a different architecture (e.g. separate I2S buses per chip without stereo mapping).

**Pin budget for the host** (GPIO / peripheral pads only, not analog coupling components): see **`MCU_VS_PI_ZERO_STACK.md`** → section **Pin requirements (GPIO count)**.

### Analog line levels (WM8731 ↔ AG1170 VIN / VOUT)

- **ADC full-scale (line in):** **1.0 × (AVDD / 3.3) Vrms** → **~1.0 Vrms** at **AVDD = 3.3 V**. Above that risks **overload / clipping**; full scale **tracks AVDD**.
- **DAC full-scale (line out):** same scaling to **~1.0 Vrms** at **AVDD = 3.3 V** (into a **10 kΩ** load condition in the spec tables).
- Line inputs are **AC-coupled** in the reference application; datasheet also shows **resistive dividers** when feeding a hotter source (e.g. **2 Vrms** CD player attenuated toward **1 Vrms**).

**Design actions**

- Use **AC coupling** (e.g. **1 µF** film/ceramic + biasing per Wolfson “line input application”) between **AG1170 VOUT/VIN** and **WM8731 LLINEIN/RLINEIN / LOUT/ROUT**, consistent with Silvertel AG1170 (**connect VIN/VOUT to the codec via a 100 nF capacitor** per datasheet). Your BOM may add **larger caps** or Silvertel app-note parts — follow the **AG1170** applications drawings in the full PDF.
- **Measure** peak VOUT from the SLIC in worst-case (ring trip, loud room); if peaks clip the ADC, add a **resistive pad** (divider) or reduce codec **line input gain** in software.

---

## 1B. ES8388 + AG1171 (SMD2/SMD3 production board) — measured DTMF clipping, 2026-08

**Symptom:** real DTMF dialing on a physical handset (MaxPhone, DualPhone —
both SMD2, confirmed identical circuit) intermittently failed to register
digits. Root-caused to hard ADC clipping, not a detector bug — captured
live via a temporary `peak` diagnostic in `dtmf_detector_push()`
(`firmware/components/dtmf/dtmf.c`): **every correctly-classified digit
showed `peak=32768`**, i.e. int16 full-scale, on both boards.

### Datasheet numbers (`datasheets/ES8388.pdf` §8.3, "ADC Analog and Filter
Characteristics")

| Parameter | Value | Notes |
|---|---|---|
| Full Scale Input Level | **AVDD/3.3 Vrms** → **1.0 Vrms** at AVDD = 3.3 V (this board) | RMS of a single-tone test signal — see crest-factor caveat below |
| Input Impedance | **20 kΩ** | Divider sizing needs to account for this loading, not just the nominal ratio |

(A `pdftotext -layout` extraction of this PDF now exists alongside the
other `*_extracted.txt` files — see §6 below.)

### Measured levels (oscilloscope, DualPhone, wire tapped on the AC side of
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

### Ruled out: codec gain

`micamp 0 0` (ES8388 ADC PGA at its floor, 0 dB — console command,
`firmware/components/bringup/bringup.c`) still clips identically on every
digit. This proves the problem is upstream of the codec's programmable
gain stage entirely — the raw analog signal already exceeds the ADC's
input range before the codec's own gain has any say. Not fixable in
software; needs an analog attenuator between AG1171 VOUT and the ES8388
input pin.

### Recommended divider (final V3 values — see "Built and confirmed working" below)

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

### Re-tuning after the hardware fix

`micamp 0 0` was diagnostic only, to isolate codec-gain from the real
(upstream) problem. Turned out to be moot in practice: `micamp` is a live
I2C register poke, not persisted to NVS, so it reverts to the firmware's
+3dB/+3dB boot default on any reset regardless — and the divider holds off
clipping fine at that default gain (see "Built and confirmed working"
above), so no further gain re-tuning was needed.

**MaxPhone:** now has the same 10 kΩ/8.2 kΩ rework applied (confirmed same
circuit as DualPhone). MaxPhone has a rotary handset (no keypad), so the
`tonemon` + `peak=` DTMF verification above doesn't apply there directly —
its line has only been exercised via pulse dialing so far, not confirmed
against a `peak=` reading the way DualPhone's DTMF path was.

---

## 2. Silvertel AG1170 (full datasheet)

### Module supply

- **+VPWR / +VA:** **+5 V or +3.3 V** (single supply to the module’s DC/DC and analog front end, per **ordering code** — e.g. **-S3 / -D3** vs **-S5 / -D5** style parts in the datasheet ordering table).

### Analog codec interface (AG1170)

- **VIN:** audio **from codec → SLIC** (toward tip/ring). Datasheet: **connect via a 100 nF capacitor**.
- **VOUT:** audio **from SLIC → codec** (from tip/ring). Same **100 nF** coupling to the codec.
- **Gain:** nominally **0 dB** tip/ring ↔ VOUT/VIN; use **codec / DSP** for level tweaks.
- Validate peaks against **WM8731 ~1 Vrms** line full-scale (see §1).

### Control pins (F/R, RM, PD, SHK) — **logic vs Pi / ESP32**

Pin descriptions (AG1170 text):

- **F/R (forward/reverse):** **Logic 1** = forward battery sense (datasheet: Tip **positive** with respect to Ring when F/R held at logic 1); **logic 0** reverses tip/ring polarity. Used for ringing cadence (toggle) and battery reversal features.
- **RM (ringing mode):** **Logic 1** during ringing; **logic 0** in other modes. Do not leave timing violations vs **SHK** debounce (see datasheet timing for ring trip).
- **SHK:** **Logic 1** = **off-hook** indication (output from SLIC to MCU). **De-bounce** in software (datasheet recommends persistence on the order of **10 ms** class behavior — confirm in PDF timing section).
- **PD (power-down):** **Logic 0** powers down the DC/DC converter. Datasheet: **do not** apply a **logic 1** to PD; if power-down is not needed, **leave PD open circuit**. Use an **open-collector** drive if the MCU must pull PD active; for **polling** power-down modes, the sheet references a **square wave** on PD (e.g. **64 kHz @ 3.3 V** context in the narrative — read the full **PD** section before copying the waveform).

**Inputs to the SLIC (MCU → F/R, RM, PD):**

- Use the **DC electrical characteristics** tables in **`AG1170_Silvertel_datasheet.pdf`** for **V_IL / V_IH** at your **VPWR**. **Rule of thumb:** a **3.3 V** MCU driving **~3.0 V high** is often a valid **high** into a **5 V** VPWR part, but **always** confirm from the table.

**Outputs from the SLIC (SHK → MCU):**

- At **3.3 V VPWR**, **SHK** high/low levels are usually **safe** to read with **3.3 V** CMOS inputs — still use the **PDF tables** (extract spacing is unreliable).

**If VPWR = 5 V:**

- **SHK** may swing **near 5 V** when high. **Pi GPIO is not 5 V-tolerant.** Use a **divider**, **level shifter**, or other **5 V → 3.3 V** input conditioning on **SHK** (and any other SLIC output to the MCU).

---

## 3. Pi Zero 2 W and ESP32 (host)

- **I/O voltage:** **3.3 V CMOS** (no 5 V I2S).
- **I2S lines (BCLK, LRCK, DIN, DOUT):** keep on **3.3 V**; **series resistors** (e.g. **33 Ω–100 Ω**) often help **SI / EMI** to the codec.
- **I²C pull-ups:** **3.3 V** to **WM8731**; typical **2.2 kΩ–4.7 kΩ** (one set per bus).

---

## 4. Recommended architecture (minimizes level headaches)

| Choice | Why |
|--------|-----|
| **One 3.3 V “digital + codec analog” island** | **WM8731** max supply **3.63 V**; **Pi / ESP32** are **3.3 V**. |
| **Silvertel **3.3 V** module variant (e.g. AG1170‑S3 / ‑D3 class)** | Aligns **SHK / F/R / RM** with **3.3 V** MCU and codec **DBVDD** (verify **V_OH** / **V_IH** in **`AG1170_Silvertel_datasheet.pdf`**). |
| **5 V SLIC only if needed** | Then: **MCU → SLIC** inputs often OK without shifters; **SLIC → MCU** (**SHK**) almost always needs **down-leveling**. |
| **Separate high-voltage / line-side domain** | Tip/ring is **isolated / high impedance** from your logic — follow Silvertel **protection** and **overvoltage** app notes; this doc does **not** replace telecom safety review. |

---

## 5. I2S voltage (there is no “5 V I2S” here)

**I2S is digital CMOS** at the codec’s **DBVDD** (your design: **3.3 V**). There is **no analog “I2S voltage”** — only **strict timing + 3.3 V logic levels**. **Do not** connect **5 V** logic to **WM8731** digital pins.

---

## 6. Files in `datasheets/`

| File | Purpose |
|------|---------|
| `WM8731_Wolfson_audio_CODEC.pdf` | Original WM8731 PDF |
| `WM8731_Wolfson_audio_CODEC_extracted.txt` | **Searchable** text dump (extraction imperfect on figures). |
| `AG1170_Silvertel_datasheet.pdf` | **Silvertel AG1170** full datasheet (canonical). |
| `AG1170_Silvertel_datasheet_extracted.txt` | **Searchable** text dump (`pypdf`; tables may be mis-spaced). |
| `AG1171_Silvertel_datasheet.pdf` | **AG1171** sibling part — canonical for §1B (SMD2/SMD3 uses AG1171, not AG1170). |
| `AG1171_Silvertel_datasheet_extracted.txt` | Text dump for grep / comparison. |
| `ES8388.pdf` | **ES8388** codec (SMD2/SMD3) full datasheet — see §1B. |
| `ES8388_extracted.txt` | Text dump (`pdftotext -layout`; §8.3 "ADC Analog and Filter Characteristics" has the input-level spec). |

**Regenerate extractions** (optional): project venv `.venv-datasheet` with `pypdf` was used once to build `*_extracted.txt`; you can delete that venv if you do not want it on disk. `ES8388_extracted.txt` was built directly with `pdftotext -layout` instead (simpler for a one-off, same idea).

---

## 7. Quick checklist before spin

- [ ] **WM8731** **AVDD / HPVDD / DBVDD** at **3.3 V**; **DCVDD** per reference (often **1.5–1.8 V**) with **DCVDD ≤ AVDD**.
- [ ] **No 5 V** on **WM8731** or **Pi / ESP32** I2S / I²C.
- [ ] **AG1170 VPWR** variant matches your **SHK** level plan (**3.3 V** preferred for single domain).
- [ ] **SHK → MCU** verified if **VPWR = 5 V** (**divider or translator**).
- [ ] **PD** pin wired per Silvertel (**never illegal high**).
- [ ] **AC coupling + optional pad** on **VIN/VOUT** to codec; **measure** clipping margins.
- [ ] **SMD3 (next spin):** lay out the SLIC→codec audio path with a real divider footprint (series R + shunt-to-ground R pads after the coupling cap, per §1B) from the start — SMD2 needed a desolder-the-cap bodge to add this after the fact because the cap footprint sat with no room for a 3rd component in series.

---

*This document is engineering guidance, not a substitute for the full manufacturer datasheets or compliance review.*
