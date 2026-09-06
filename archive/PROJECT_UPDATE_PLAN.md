# Project update plan: networked analog phone (Pi Zero + Silvertel + codec)

## Goal

Build a small box around a **Raspberry Pi Zero (W)** with **Silvertel AG1170** (SLIC / line interface) and a proper **audio codec** so a **standard analog phone** behaves like a **networked VoIP phone**: ring, dial tone, full-duplex voice to another extension or IP-based peer, without relying on USB sound dongles for the line audio path.

---

## Audio chain (high level)

Analog phone audio stays on the AG1170; the Pi needs **digital I2S**, so the chain is:

```text
Phone ↔ AG1170 (analog) ↔ VIN / VOUT ↔ Codec (ADC + DAC) ↔ I2S ↔ Pi Zero
```

- **VIN**: Pi → phone (transmit to line)
- **VOUT**: phone → Pi (receive from line)

Telephone audio is **narrowband (~300 Hz–3.4 kHz)**; codec fidelity beyond “telephone grade” is optional.

---

## Codec options (from research)

| Approach | Parts | Notes |
|----------|--------|--------|
| **A – Split** | **PCM5102A** (DAC) + **PCM1802** (ADC) | Cheap, common, hand-solderable; breakouts exist for quick prototypes. |
| **B – Single codec** | **WM8731**, **TLV320AIC3104**, **CS4270**, etc. | One chip for ADC+DAC; fewer parts. |

**Pi Zero I2S:** One hardware I2S bus (typical GPIO **18 / 19 / 20 / 21**).

**Two phone lines on one Pi Zero**

- **Stereo codec, L/R split:** One line on **left** I/O, one on **right** I/O — **one I2S interface**, clean for **exactly two** AG1170 modules.
- Alternatives for more lines: TDM-capable codec (e.g. TLV320AIC3104), or second I2S (software/bit-bang on Pi Zero is possible but not ideal).

**Chosen direction for 2× AG1170 + Pi Zero:** **WM8731** (stereo codec, kernel ALSA driver, QFP hand-solderable, good community docs).

**Full duplex per line (2 lines = 4 logical paths)**

| Line | Pi → phone (DAC) | Phone → Pi (ADC) |
|------|------------------|------------------|
| 1 | DAC **L** → AG1170 #1 **VIN** | ADC **L** ← AG1170 #1 **VOUT** |
| 2 | DAC **R** → AG1170 #2 **VIN** | ADC **R** ← AG1170 #2 **VOUT** |

WM8731 fits **2 DAC + 2 ADC** channels in one package on **one** I2S bus.

**WM8731 input mux caveat:** ADC path mux is **shared** across channels (mic vs line). Use **line-in mode for both** — appropriate for AG1170 **VOUT**.

**Levels:** Confirm **AG1170 VOUT** vs **WM8731 line input** full-scale (~order of **1 Vrms**); add a **resistor divider** if the module output is too hot.

---

## Hardware build checklist

- [ ] Pi Zero W (or **Pi Zero 2 W** for more CPU headroom)
- [ ] 1× or 2× **AG1170** (or Silvertel module) per line
- [ ] **WM8731** (+ support passives per datasheet / reference design)
- [ ] I2S + I2C to Pi; analog routing AG1170 ↔ codec L/R as above
- [ ] GPIO for **hook (SHK)**, **ring / feed** control lines as required by AG1170 and your existing `telephone.py` conventions (update pin map for new PCB)
- [ ] Power / isolation per Silvertel and safety rules for telephone interfacing

---

## VoIP / PBX software plan

**Split roles (important on Pi Zero 512 MB RAM)**

- **PBX:** Run **Asterisk** on a **separate always-on host** (e.g. Raspberry Pi 4, home server, or small **VPS**). Do **not** plan to run full Asterisk on the Pi Zero.
- **On each Pi Zero:** Lightweight **SIP user agent** + your **GPIO / hook / ring** glue + **ALSA** to the WM8731 device.

**SIP client options**

- **baresip** — preferred for embedded/headless: light RAM, scriptable control (off-hook → dial, incoming → ring).
- **Linphone** (`linphonec`) — common; heavier than baresip on a Zero.

**Minimal Asterisk idea**

- Extensions **101** / **102** map to the two SIP peers (two Pi phones).
- Simple dialplan: dial **101** rings phone 1, **102** rings phone 2.

**Calling outside the home LAN (NAT)**

| Option | Idea |
|--------|------|
| **VPS Asterisk** | Both Pis register **outbound** to VPS — fewest NAT headaches. |
| **Home Asterisk + port forward** | Forward **UDP 5060** (SIP) and **UDP 10000–20000** (RTP); use **DDNS** if dynamic IP; tune Asterisk NAT (`force_rport`, `comedia`, etc.) for one-way audio. |
| **Tailscale** | Asterisk + all Pis on Tailnet — **no public port forward**; remote Pi acts like LAN; good for a **small trusted** deployment. |

---

## Software flow (per Pi Zero, conceptual)

1. Register **baresip** (or chosen UA) to Asterisk (or peer).
2. Monitor **hook GPIO** (and pulse counting for rotary if kept).
3. **Off-hook:** dial tone (local or from PBX), collect digits / extension, place SIP call.
4. **Incoming SIP:** drive **AG1170 ring** until answered; **off-hook** stops ring and answers.
5. **RTP audio** ↔ **ALSA** default device = **WM8731** (stereo: L = line 1, R = line 2 if dual-line firmware).

---

## Relation to this repo

Current `telephone.py` assumes **USB sound cards** (`amixer -c 2/3`) and **GPIO** for ring / hook / pulse. The update plan replaces line audio with **I2S + WM8731 + AG1170** and adds a **SIP stack** for real VoIP; GPIO and call-state logic can evolve into the glue layer above baresip/Asterisk.

---

## Open next steps

1. Schematic: **AG1170 ↔ WM8731** analog and grounds; Pi **I2S + I2C**; GPIO ring/hook.
2. **Device tree / overlay** or module probe for WM8731 on chosen pins (if not plug-and-play HAT).
3. **baresip** config + minimal **Asterisk** `pjsip.conf` / `extensions.conf` for two extensions.
4. Prototype **one line** before locking stereo routing for two.

---

*Summarized from design chat (Silvertel AG1170, WM8731, Asterisk, baresip, Tailscale/VPS options).*
