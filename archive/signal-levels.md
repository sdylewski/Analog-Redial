# Signal levels — ES8388 + AG1171 + ESP32-C6

Power, logic, and audio level reference for the SMD4 board (ESP32-C6 +
ES8388 codec + AG1171 SLIC). Derived from the ES8388 and AG1171 datasheets
(local copies in `datasheets/`).

This document is engineering guidance, not a substitute for the full
manufacturer datasheets or a telecom safety/compliance review.

---

## 1. Supply domains

| Rail | Value | Feeds |
|------|-------|-------|
| **+5 V** | From USB-C or DC jack (diode-ORed) | AG1171 `+VPWR`, ESP32-C6-DevKit's own onboard 5V→3.3V regulation |
| **+3.3 V** | From the DevKit's own regulator | ES8388 (all supply pins), I²C pull-ups, microSD |

ES8388 absolute maximum on its supply pins is **3.63 V** — never run it from
5 V. The AG1171 SLIC runs from the **+5 V** rail; its logic control pins
(SHK, F/R, RM, PD) need level handling against the ESP32's 3.3 V CMOS GPIO
(see below).

---

## 2. SLIC control pins vs. 3.3 V GPIO

| Pin | Direction | ESP32 side |
|-----|-----------|------------|
| **SHK** (hook state) | SLIC → ESP32 | ~3.3 V logic (internal 3.3 kΩ pull-up, VOH 2.8 V min), not 5 V. SMD4's 10 kΩ / 15 kΩ divider over-attenuates it to ~1.9 V, below the ESP32's 2.475 V input-high threshold — use 10 kΩ / 100 kΩ (see `../docs/hardware-smd4.md`) |
| **F/R** (forward/reverse) | ESP32 → SLIC | Logic 1 = forward battery sense (normal polarity); used for ring cadence |
| **RM** (ringing mode) | ESP32 → SLIC | Logic 1 during ringing |
| **PD** (power-down) | ESP32 → SLIC | **Never drive a logic-1 directly** — datasheet requires PD be left open-circuit or driven through a diode (see `../docs/hardware-smd4.md`); a GPIO output low pulls PD low (power-down), driving high lets the diode block and PD floats (run) |

---

## 3. Audio coupling (AG1171 ↔ ES8388)

- **AG1171 VIN** (codec → SLIC, toward tip/ring): 10 nF series coupling cap.
- **AG1171 VOUT** (SLIC → codec, from tip/ring): 100 nF series coupling cap.
- Nominal gain is 0 dB tip/ring ↔ VOUT/VIN — use codec gain registers for
  level tuning, not passive attenuation, unless headroom requires it (next
  section).
- ES8388 ADC full-scale input: **AVDD/3.3 Vrms → ~1.0 Vrms** at AVDD = 3.3 V.
  ES8388 input impedance: **20 kΩ**.

### DTMF headroom

DTMF is a summed dual-tone signal — its instantaneous peak-to-peak swing
runs well above what a same-RMS single tone (like a dial tone) would
produce, because of its crest factor. A real handset's DTMF digit can clip
the ES8388's ADC input even when a naive single-tone RMS estimate looks like
it should fit under the 1.0 Vrms full-scale spec. Symptom if this is
happening on your board: dial tone plays and sounds fine, but real DTMF
digits from a physical handset misclassify or drop intermittently, while a
firmware-generated test tone (`dtmf_gen`) still decodes correctly (since it
never reaches the same physical clipping path).

If you see that symptom, add a resistive divider **after** the SLIC→codec
coupling cap, close to the codec pin (not directly across the SLIC's VOUT —
that loads the SLIC's output stage and fights the cap's DC-blocking job):
**~10 kΩ series, ~8.2 kΩ shunt-to-ground**, targeting roughly 1.0–1.2 Vpp at
the codec pin from a ~2.5 Vpp DTMF signal. Keep the codec's mic PGA gain at
**0 dB** (the ES8388 power-on-reset default) — raising it drops the chip's
effective input impedance enough to swamp a fixed shunt resistor and
attenuate real speech through the same divider, even though louder DTMF
still gets through fine.

---

## 4. Quick checklist before you build

- [ ] ES8388 all supply pins at 3.3 V — never 5 V.
- [ ] AG1171 `+VPWR` on the 5 V rail; SHK divider is 10 kΩ / **100 kΩ** (not 15 kΩ) — off-hook should measure ≥2.6 V at the GPIO.
- [ ] PD never driven to a hard logic-1 — diode or open-circuit only.
- [ ] AC coupling on VIN/VOUT per `../docs/hardware-smd4.md`; if real handsets misdetect
      DTMF, add the divider above.
- [ ] Mic PGA gain left at 0 dB unless you've re-verified headroom at a
      higher setting.

## See also

- [`../docs/hardware-smd4.md`](../docs/hardware-smd4.md) — schematic-level pin map and coupling values
- [`docs/bom.md`](../docs/bom.md) — part numbers
