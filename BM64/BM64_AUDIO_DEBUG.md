# BM64 Audio Debug — Why AOHPL Is Silent

## Date: 2026-09-26

## Root Cause (very likely)

The EEPROM table that was flashed (`MCHP_DSPKv2.1.7_BM64CLS1.txt`) routes
A2DP audio to the **I2S pins**, not to the analog AOHPL/AOHPR DAC. The repo
docs had the two tables labeled backwards.

### Evidence

Decoded from `BT5506_012UI_Parameter_Table_R0.xls` (Microchip's official
parameter table, in `DSPK+v2.1.7+Package/Tools/UI Tool/`):

**EEPROM `0x1FC` bit2 = "External CODEC"**
- `0` = External codec → audio out on **I2S** (to an external DAC/DSP)
- `1` = Internal codec → audio out on **analog AOHPL/AOHPR**

**AN2614** (Bluetooth Dual-Mode Speaker Audio Application Note) confirms:
> "External Codec – audio is routed to I2S"
> "Internal Codec – audio is routed to SPK"
> "BM64 is used for applications that require an I2S digital output."

The BM64 EVB feeds I2S into a Yamaha DSP amp (YDA174), which is why the
"CLS1" table defaults to external codec / I2S output.

### The two tables compared

Only **3 bytes differ** between CLS1 and CLS1_SPKOUT:

| Byte | CLS1 (flashed) | CLS1_SPKOUT | Parameter Table Field | Meaning |
|------|-----------------|-------------|----------------------|---------|
| `0x1FC` | `0x4A` (bit2=0) | `0x06` (bit2=1) | dsp_option2, bit2 "External CODEC" | **I2S vs analog** |
| `0x094` | `0x73` | `0x13` | shs_action_option12, bit5+bit6 | Audio SRC + Voice SRC (on for I2S, off for analog) |
| `0x171` | `0x46` | `0x40` | shs_action_option13, bit1+bit2 | Voice Stereo + Tone Stereo |

### What the repo docs said (WRONG)

```
CLS1       = "Headphone DAC on A0HPL/A0HPR"     ← WRONG, this is I2S
CLS1_SPKOUT = "Class D amp on SPK_P/SPK_N"      ← WRONG, this is analog AOHP DAC
```

The BM64 has **no Class-D amplifier**. Its "speaker output" (what Microchip
calls "SPK") is the AOHP headphone DAC in either capless or single-ended
mode. The `_SPKOUT` suffix means "route to the internal codec's analog
speaker output" — i.e. AOHPL/AOHPR.

---

## Pin Numbers Were Also Wrong

The repo doc (`BM64_Analog_Audio_Setup.md`) listed:
```
A0HPL  = pin 24    ← WRONG (pin 24 = SYS_PWR)
A0HPR  = pin 23    ← WRONG (pin 23 = ADAP_IN)
MIC_P1 = pin 19    ← WRONG (pin 19 = HCI_RXD)
```

**Correct BM64 pin numbers** (from datasheet Table 1-3, DS60001403F):

| Pin | Name | Function |
|-----|------|----------|
| **5** | AOHPR | Right-channel analog headphone output |
| **6** | AOHPM | Headphone common-mode output/sense |
| **7** | **AOHPL** | **Left-channel analog headphone output** |
| **8** | MIC_N1 | Mic differential negative input |
| **9** | MIC_P1 | Mic differential positive input |
| 10 | MIC_BIAS | Electret mic bias voltage |
| 11 | AIR | Right-channel single-ended analog input |
| 12 | AIL | Left-channel single-ended analog input |
| 19 | HCI_RXD | HCI-UART data input |
| 20 | HCI_TXD | HCI-UART data output |
| 22 | BAT_IN | Power supply (3.2–4.2 V) |
| 23 | ADAP_IN | 5 V adapter input (charging) |
| 24 | SYS_PWR | System power output |
| 26 | MFB | Multi-function button / UART wakeup |

**Action:** verify the PCB net labeled AOHPL actually goes to BM64 **pin 7**,
not pin 24. If the footprint used the wrong pin numbers, AOHPL may be
connected to a power pin.

---

## Capless vs Single-Ended Mode

### Which mode to use: Single-Ended

Our circuit AC-couples AOHPL into a fork (ES8388 line-in + AG1171 VIN),
both of which are ground-referenced high-impedance inputs. This is
**single-ended** mode.

**Why not capless:** capless mode returns the load current to AOHPM (a
mid-supply common-mode pin), not to ground. It can't drive ground-referenced
inputs like the ES8388 and AG1171.

### EEPROM settings for single-ended mode

| Address | Field | Required Value | Current Value | Status |
|---------|-------|---------------|---------------|--------|
| `0x1D7` | CODEC_STEREO_DAC_path bit4 | `1` (Single End) | `0x94` → bit4=1 | ✓ OK |
| `0x1D8` | CODEC_MONO_DAC_path bit4 | `1` (Single End) | `0x93` → bit4=1 | ✓ OK |
| `0x348` | codec_parameter[0] bits2:0 | `010` (single-end with earphone) | Was `0x09` (capless), rewritten to `0x0A` | ✓ Fixed |

**If the DAC's idle low-power behavior causes dropouts or pops later:**
`0x348` = `0x0E` is the documented alternative ("single-end with external
always-on AMP" — keeps the DAC powered continuously).

### Coupling cap value

The datasheet characterizes single-ended mode with a 47 µF cap into 16 Ω
(headphone). Our loads are high-impedance (10 kΩ+), so 1–10 µF is plenty.
1 µF into 10 kΩ gives a −3 dB corner around 16 Hz.

**Use one cap per branch** (one to ES8388, one to AG1171). Each downstream
input has its own DC bias, and a shared node lets those biases fight.

---

## Firmware Version

- **Package:** DSPK v2.1.7 (latest available).
- **Flash image:** `BT5506_SHS_FLASH_Rehex_0E14.HEX` — used with one of
  the BM64 EEPROM tables.
- **Module ROM:** reports as `5506_DUAL_SPK`, version `0201`. Matches the
  table header.
- **No firmware reflash needed** for this test. The fix is EEPROM-only.
- **Note (v2.1.2+):** the internal codec drops into low-power mode when
  idle. Only expect signal at AOHPL while audio is actually streaming.
- **Note:** `0x1FD` bit4 = "Power On by Power On Command" is set. This is
  why the `0x51`/`0x52` MMI POWERON_BUTTON_PRESS/RELEASE pair is needed
  before pairing.

---

## The Fix: 3 EEPROM Bytes

Switch from the CLS1 (I2S) values to the SPKOUT (analog) values:

```
bm64_eeprom write 01FC 06    (bit2=1 → Internal codec / analog output)
bm64_eeprom write 0094 13    (SRC off — not needed for internal codec)
bm64_eeprom write 0171 40    (voice/tone stereo off — match SPKOUT table)
```

Leave `0x348` at `0x0A` (single-ended, already written). Leave `0x1D7`/
`0x1D8` alone (already correct for single-ended).

Read each one back to confirm:
```
bm64_eeprom read 01FC 1    → expect 06
bm64_eeprom read 0094 1    → expect 13
bm64_eeprom read 0171 1    → expect 40
bm64_eeprom read 0348 1    → expect 0A
bm64_eeprom read 01D7 1    → expect 94 (unchanged)
```

---

## Test Plan: BM64 Audio on SMD4 Board

### 1. Hardware checks (power off)

- [ ] Verify AOHPL net goes to BM64 **pin 7** (not pin 24).
- [ ] Lift coupling cap(s) to SLIC and ES8388 — test the BM64 DAC alone.
- [ ] Attach temporary load: 10 µF cap → 10 kΩ to GND (or earbuds through
      47–100 µF cap).

### 2. Power check (board running)

- [ ] Measure BAT_IN (pin 22): must stay **≥ 3.2 V**. Our 3.3 V rail is
      near the minimum spec. A voltage sag under load could brown out the
      analog section.

### 3. Read current EEPROM state

```
bt_enable 0
bm64_ibdk
bm64_eeprom read 01FC 1
bm64_eeprom read 0094 1
bm64_eeprom read 0171 1
bm64_eeprom read 01D7 1
bm64_eeprom read 0348 1
```

### 4. Write the analog-output bytes

```
bm64_eeprom write 01FC 06
bm64_eeprom write 0094 13
bm64_eeprom write 0171 40
```

Read each back to confirm.

### 5. Return to normal mode and pair

```
bm64_ibdk exit
bt_enable 1
bm64_pair
```

May need to forget the device on the phone first (the name/capabilities
may have changed).

### 6. Signal test

- Phone volume all the way up.
- Play a **1 kHz test tone** (not music — easier to see on scope).
- **Scope BM64 pin 7 directly:** expect DC bias (~VBAT/2) with the tone
  riding on top.
- **Scope load side of the coupling cap:** expect tone centered on 0 V.
- Also try AOHPR (pin 5).

### 7. Reconnect downstream (only after step 6 passes)

- Reconnect ES8388 line-in first → verify with `capture` command.
- Then reconnect AG1171 VIN → verify with `hookmon`.

### 8. If still no signal at pin 7

- Set `log_level debug` — confirm A2DP streaming events appear.
- Try sending the volume command manually:
  `bm64_mmi 23 00 00 05 7F 7F 7F` (set all volumes to max).
- Check that AOHPM (pin 6) shows a stable DC bias (~VBAT/2). If AOHPM is
  floating or at 0 V, the analog output stage may not be powered.
- As a last resort, try `0x348` = `0x0E` (always-on AMP mode) in case the
  codec's idle low-power mode is preventing the DAC from waking up.

---

## Related Files

| File | Purpose |
|------|---------|
| `BM64/BM64_Analog_Audio_Setup.md` | Main setup doc (pin numbers + table labels being fixed) |
| `BM64/eeprom_write/eeprom_write.md` | EEPROM protocol reverse-engineering notes |
| `DSPK+v2.1.7+Package/Tools/UI Tool/.../BT5506_012UI_Parameter_Table_R0.xls` | Official parameter table |
| `DSPK+v2.1.7+Package/Software/EEPROM Table/MCHP_DSPKv2.1.7_BM64CLS1.txt` | I2S config (was flashed) |
| `DSPK+v2.1.7+Package/Software/EEPROM Table/MCHP_DSPKv2.1.7_BM64CLS1_SPKOUT.txt` | Analog config (need these 3 bytes) |
| `BM62-BM64-Data-Sheet-60001403F.pdf` (in Documents/BM64/) | Pin tables, electrical specs, output mode diagrams |
| `Bluetooth-Dual-Mode-Speaker-Audio-Application-00002614D.pdf` | AN2614 — codec setup, I2S vs SPK routing |
| `BM6x DSPK 2.1.7 Release Notes.pdf` | Firmware changelog, codec low-power note |
