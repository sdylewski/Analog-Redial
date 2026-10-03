# Power, logic, and audio levels — through-hole board (WM8731 + AG1170)

This note is derived from the **local datasheet copies** in `datasheets/`
(text extractions: `*_extracted.txt`). Numbers cite **Wolfson WM8731**
production data **Rev 3.4, April 2004** and **Silvertel AG1170** full
datasheet (local PDF; revision/date on cover sheet — see extract).

Split out from the old `HARDWARE_SIGNAL_LEVELS.md` on 2026-10-03 — that
file covered both this board and the SMD2/SMD4 lineage in one doc; its
SMD2-specific sections (ES8388/AG1171 DTMF-clipping rework, AEC timing)
moved to [`../smd2.md`](../smd2.md) instead, since they're unrelated to
this board. The current production board's own signal-level reference is
[`../../../archive/signal-levels.md`](../../../archive/signal-levels.md)
(SMD4, ES8388 + AG1171).

---

## 1. WM8731 (codec)

Pin-by-pin functional guide (KiCad **U1** symbol, gateway wiring): **`hardware/through-hole-wm8731/WM8731.md`**.

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

---

## 6. Files in `datasheets/`

| File | Purpose |
|------|---------|
| `WM8731_Wolfson_audio_CODEC.pdf` | Original WM8731 PDF |
| `WM8731_Wolfson_audio_CODEC_extracted.txt` | **Searchable** text dump (extraction imperfect on figures). |
| `AG1170_Silvertel_datasheet.pdf` | **Silvertel AG1170** full datasheet (canonical). |
| `AG1170_Silvertel_datasheet_extracted.txt` | **Searchable** text dump (`pypdf`; tables may be mis-spaced). |

**Regenerate extractions** (optional): project venv `.venv-datasheet` with `pypdf` was used once to build `*_extracted.txt`; you can delete that venv if you do not want it on disk.

---

## 7. Quick checklist before spin

- [ ] **WM8731** **AVDD / HPVDD / DBVDD** at **3.3 V**; **DCVDD** per reference (often **1.5–1.8 V**) with **DCVDD ≤ AVDD**.
- [ ] **No 5 V** on **WM8731** or **Pi / ESP32** I2S / I²C.
- [ ] **AG1170 VPWR** variant matches your **SHK** level plan (**3.3 V** preferred for single domain).
- [ ] **SHK → MCU** verified if **VPWR = 5 V** (**divider or translator**).
- [ ] **PD** pin wired per Silvertel (**never illegal high**).
- [ ] **AC coupling + optional pad** on **VIN/VOUT** to codec; **measure** clipping margins.

---

*This document is engineering guidance, not a substitute for the full manufacturer datasheets or compliance review.*
