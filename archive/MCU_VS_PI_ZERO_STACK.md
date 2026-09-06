# Stack: ESP32 vs Raspberry Pi Zero **2** W (dual RJ11 phone gateway)

This doc assumes the host is **either** a **Raspberry Pi Zero 2 W** (Linux SIP endpoint) **or** an **ESP32-class** module (MCU SIP endpoint). Both attach to the same analog front end: up to **two** Silvertel **AG1170** SLICs and one **stereo I2S codec** (e.g. **WM8731**) so **L/R** maps to **two independent phone lines**.

It is **not** about running **Asterisk** on the box itself: the PBX stays on another machine or VPS.

---

## Product shape (one small enclosure)

**Connectors**

- **Two RJ11** jacks (line 1 and line 2), wired to **two AG1170** channels (tip/ring to each jack).
- **Power** (e.g. 5 V USB barrel or USB-C, plus any isolated rails the SLICs need per Silvertel).
- **No Ethernet required** if you are happy with **Wi‑Fi** (both hosts have Wi‑Fi).

**Operating modes (software policy, same hardware)**

| Mode | What you plug in | Behavior |
|------|------------------|----------|
| **Two local phones** | Phones on **both** RJ11 | Two extensions on one Wi‑Fi SIP endpoint: stereo codec **L = line 1**, **R = line 2**; GPIO tracks **two** hooks / ring drives. |
| **One local + VoIP** | Phone on **one** RJ11 | Unused line idle; active line does voice to/from RTP while the other port can be empty or a second local phone later. |
| **Both** | Two phones + PBX | Each line can place/receive calls through the same Asterisk (or provider); routing is dialplan + your glue (which line answered, etc.). |

The **hardware** is the same in all cases; **firmware/Linux config** decides whether one or two SIP registrations, merged dial tone, etc.

---

## Audio + SLIC block diagram

```text
RJ11-1 ──► AG1170 #1 ── VIN/VOUT ──► Codec **L** (DAC out / ADC in)
RJ11-2 ──► AG1170 #2 ── VIN/VOUT ──► Codec **R** (DAC out / ADC in)
                        I2S + I2C ◄──► Pi Zero 2 W  or  ESP32
                        GPIO ◄──────── hook / ring / dial (per line)
```

**Codec:** One **WM8731** gives **2× DAC + 2× ADC** on one I2S link — enough for **two full-duplex** analog lines. Configure WM8731 ADC mux for **line-in** on both channels (shared mux setting). The **mono MICIN** is a separate path; it does **not** mean you lose a line channel — for two SLICs you use **LLINEIN + RLINEIN** and **LOUT + ROUT** only (see **`HARDWARE_SIGNAL_LEVELS.md`**).

**Datasheets (local copies)** in `datasheets/`: **`WM8731_Wolfson_audio_CODEC.pdf`**, **`AG1170_Silvertel_datasheet.pdf`**, and (optional sibling) **`AG1171_Silvertel_datasheet.pdf`**. Searchable text dumps: `*_extracted.txt`. **Supplies, SHK levels, and I2S logic:** see **`HARDWARE_SIGNAL_LEVELS.md`**.

---

## Pin requirements (GPIO count)

Counts are **MCU ↔ digital logic** only. **Analog** VIN/VOUT run **AG1170 ↔ WM8731** (not to the Pi/ESP32). **Wi‑Fi / USB** use pins inside the module or dedicated pads — not included here.

### One WM8731 + two AG1170 (recommended 2-line box)

| Block | Interface | Signals (typical) | MCU pins | Notes |
|-------|-----------|-------------------|----------|--------|
| **WM8731** | **I2S** | BCLK, LRCK (frame), DOUT (to codec DAC data), DIN (from codec ADC data) | **4** | Often **ADCLRC** and **DACLRC** are **tied together** on the PCB to the SoC’s one LR clock line → still **four** wires from host: BCLK + LR + DIN + DOUT. |
| **WM8731** | **MCLK** (optional source) | MCLK to `XTI/MCLK` | **0–1** | **0** if a **crystal** or **codec-generated** clock feeds WM8731; **+1** GPIO if the Pi/ESP32 bit-bangs / clocks MCLK (common on Pi overlays). |
| **WM8731** | **Control** | I²C **SDA**, **SCL** | **2** | **MODE** strapped to select **I²C** (no MCU pin). |
| **WM8731** | **Control (alt.)** | 3-wire SPI: `SCLK`, `SDIN`, `CSB` + `MODE` | **4** (+ strap) | Only if you choose SPI instead of I²C. |
| **AG1170 × 2** | **Per module** | **SHK** (in), **F/R** (out), **RM** (out), **PD** (out / open-drain, optional) | **3–4** each | **3** if **PD** is hardwired (always run) or only pulled low via diode network **without** a dedicated GPIO; **4** if the MCU drives **PD** safely (open-drain / per Silvertel). |
| **AG1170 × 2** | **Subtotal** | — | **6–8** | **2 × (3–4)**. |

**Typical totals (I²C WM8731, I2S + optional MCLK, 4 GPIO per SLIC)**

| MCLK from MCU? | WM8731 digital | Two AG1170 | **Total GPIO** |
|----------------|----------------|------------|------------------|
| No (crystal / other) | 4 + 2 = **6** | **8** | **14** |
| Yes | 5 + 2 = **7** | **8** | **15** |
| PD not routed to MCU (3 pins/SLIC) | 6 or 7 | **6** | **12** or **13** |

**Optional (not in the minimum SLIC table)**

| Feature | Extra MCU pins (typical) |
|---------|---------------------------|
| **Rotary dial** pulse contact (if separate from hook sense) | **+1 per line** |
| **LEDs / button** | As needed |
| **5 V SLIC, level-shifted SHK** | Same GPIO count at MCU; extra **hardware** pins for translator IC |

### If you used two WM8731 chips (usually unnecessary)

| Item | Pin impact |
|------|------------|
| Second codec | **+4** (second full I2S) or **TDM** / multiplexed bus (not standard on Pi header without custom work); **I²C** can stay **+0** if both codecs share **SDA/SCL** with **different 7-bit addresses** (requires **ADDR/CSB** strapping per chip). |
| **Pi Zero 2 W** | Only **one** convenient **I2S** instance on the usual 40-pin mapping — **two** independent I2S buses is awkward. **ESP32** has **I2S0 / I2S1** but you still pay **~4 pins per bus** plus clocking. |

For the **two RJ11 + one stereo codec** product, **plan on one WM8731** and the **~12–15 GPIO** row in the table above.

---

## Pin plan — Raspberry Pi Zero 2 W

Pi **I2S0** on the 40-pin header is the usual choice for a custom codec. **BCM** numbers below.

### I2S + I2C to WM8731

| Signal | BCM | Physical pin (40-pin) | Notes |
|--------|-----|------------------------|--------|
| **I2S BCLK** | **18** | 12 | Bit clock |
| **I2S LRCK** (WS / frame) | **19** | 35 | Left/right word clock |
| **I2S DIN** (data **to** Pi, codec ADC → SoC) | **20** | 38 | PCM_DIN |
| **I2S DOUT** (data **from** Pi, SoC → codec DAC) | **21** | 40 | PCM_DOUT |
| **I2C SDA** | **2** | 3 | `i2c-1` — WM8731 control |
| **I2C SCL** | **3** | 5 | `i2c-1` |
| **MCLK** | **4** | 7 | Many WM8731 designs wire a **GPIO clock** or crystal; **GPIO4** is commonly used in Pi overlays for codec MCLK (confirm your `dtoverlay` / board schematic). |

Align **device tree overlay** / `simple-audio-card` (or a WM8731-specific overlay) so **BCLK/LRCK/DIN/DOUT/MCLK** match the PCB. Wrong DIN/DOUT swap is a common first bring-up bug.

### GPIO per phone line (example — does not overlap I2S/I2C)

Your existing `telephone.py` used **BCM 14 / 15 / 18** for line 1; **BCM 18 is now I2S BCLK**, so **move** telecom GPIO off 18–21 and off I2C pins.

| Function | Line 1 (BCM) | Line 2 (BCM) | Notes |
|----------|----------------|--------------|--------|
| Hook / SHK (input) | **17** | **22** | Off-hook detect; debounce in software |
| Ring FR (output) | **27** | **23** | Or whatever pair your AG1170 / board uses for ring steering |
| Ring RM (output) | **24** | **25** | Same |

Pick any **free** GPIOs consistent with your PCB; the table above is one valid pattern on a **40-pin** Pi.

### Pi software stack

| Layer | Choice |
|-------|--------|
| OS | Raspberry Pi OS Lite |
| Codec | Kernel + **ALSA** (WM8731 via overlay / `simple-audio-card`) |
| SIP + RTP | **baresip** (or similar) — register to Asterisk elsewhere |
| Line control | Python or C: GPIO + stereo ALSA device (L/R = jack 1/2) |

---

## Pin plan — ESP32 (example: ESP32-WROOM-32 DevKit-class)

ESP32 **I2S pins are flexible** via the GPIO matrix; below is one **coherent** assignment that avoids **SPI flash** pins (typically **GPIO6–11**) and uses an **input-only** pin for **ADC data** from the codec.

### I2S + I2C to WM8731

| Signal | ESP32 GPIO | Notes |
|--------|---------------|--------|
| **I2S BCLK** | **5** | Output |
| **I2S WS** (LRCK) | **25** | Output |
| **I2S DOUT** (ESP → codec **DAC** data) | **26** | Serial data to WM8731 playback input |
| **I2S DIN** (codec **ADC** → ESP) | **35** | **Input-only** GPIO — valid for receive data |
| **I2C SDA** | **21** | WM8731 `SDIN` / control bus (change if your module uses other pins) |
| **I2C SCL** | **22** | |

**MCLK:** WM8731 usually needs **MCLK** at a multiple of sample rate. Options: **dedicated oscillator**, **I2S master** mode with MCLK from codec, or **LEDC** / PLL pin if your design supports it — **verify in hardware** against the WM8731 clocking section.

### GPIO per phone line (example)

| Function | Line 1 | Line 2 |
|----------|--------|--------|
| Hook / SHK | **14** | **16** |
| Ring FR | **12** | **17** |
| Ring RM | **13** | **32** |

Adjust for your module’s **strapping** pins and any **JTAG** pins you disable.

### ESP32 software stack

| Layer | Choice |
|-------|--------|
| SDK | **ESP-IDF** |
| RTOS | FreeRTOS (included) |
| Wi‑Fi | Native |
| SIP / RTP | Lean stack or port; start with **G.711** (8 kHz) |
| Audio | ESP-IDF **I2S** driver ↔ WM8731 |

---

## ESP32 vs Pi Zero 2 W

| | **Pi Zero 2 W** | **ESP32** |
|---|-----------------|-----------|
| **RAM** | **512 MB** | ~320–520 KB SRAM (part dependent) |
| **SIP UA** | **baresip** / off-the-shelf | Custom or ported **embedded SIP** |
| **ALSA / kernel codec** | Yes | No — raw I2S |
| **Dev effort** | Lower for VoIP interop | Lower BOM; **higher** firmware effort |
| **Debug** | `ssh`, `tcpdump`, logs | JTAG / UART; fewer standard VoIP tools |
| **Wi‑Fi** | Built in | Built in (typical modules) |

---

## Architectural constant

**Asterisk (or another PBX / SIP trunk)** runs on a **networked Linux host or VPS**. Each **phone box** is only an **endpoint**: SIP register + RTP audio + GPIO/SLIC control — whether that endpoint is **Pi Zero 2 W** or **ESP32**.

---

## Recommendation

- **Fast path to “real phone on Asterisk”:** **Pi Zero 2 W** + WM8731 + **baresip** + GPIO glue; use the **Pi I2S pin table** above in your PCB rules.
- **Minimum BOM / you like firmware:** **ESP32** + same analog front end; budget time for **SIP/RTP** and **MCLK** validation.

---

*Project context: dual AG1170, stereo codec, dual RJ11; host = ESP32 **or** Pi Zero 2 W only.*
