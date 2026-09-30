# Hardware Reference — SMD5 Board

> **SMD5 has not been fabricated or tested.** It is the next spin, and this
> document describes the design as drawn. For a board you can build today,
> see [`hardware-smd4.md`](hardware-smd4.md). Everything below that isn't
> marked as an SMD5 change carries over from SMD4 unchanged and is proven on
> real hardware; the SMD5-only parts are not.

Design reference for the next-generation board: a 2-layer, ESP32-C6-based
analog-phone-line gateway carrying one or two POTS lines through Silvertel
AG1171 SLICs and an Everest ES8388 stereo codec, plus an optional Bluetooth
companion.

KiCad project: `hardware/kicad/Analog Redial 5/`.

## What's new against SMD4

| Change | Why |
|--------|-----|
| **Bluetooth companion header (J2)** | 8-pin board-to-board link to a BM64 carrier — see §10 |
| **Dedicated 3.3 V LDO (U5)** for the BM64 | Keeps the module's radio bursts off the DevKit's own regulator |
| **MFB on GPIO15** with a 100 kΩ pull-down | The BM64's power button needs firmware control and must idle low |
| **SHK divider 15 kΩ → 100 kΩ** | Fixes SMD4's as-built off-hook level, which sat below the ESP32's input-high threshold |
| **Dual MCU footprint** | Takes the genuine 32-pin DevKitC-1 *or* a 30-pin AliExpress clone |
| **C10/C11 → 5 mm disc footprint, DNP** | Drops an Extended part; fit 1 µF only if you want the J7 monitor header |

## 1. Board overview

| Item | Detail |
|------|--------|
| MCU carrier | **ESP32-C6-DevKitC-1** — a complete dev board (own USB-C port, reset button, onboard 3.3 V regulator), header/socket-mounted rather than a soldered module |
| Codec | **ES8388** stereo codec, QFN-28, I²C address **0x10** (CE tied to GND) |
| SLIC | 1–2× **Silvertel AG1171**, SIL package, hand-soldered |
| Storage | Internal SPI flash (SPIFFS) always; optional **microSD** (SPI) on single-line builds |
| Power input | **USB-C** (via the DevKit's own port) **or** a **DC jack**, diode-ORed onto one +5 V rail |
| Copper | **2 layers** (`F.Cu`, `B.Cu`) |
| Board size | ~100 × 50 mm |
| Connectors | RJ12 ×1–2 (phone lines), USB-C, DC jack, microSD (optional) |

The schematic is the same circuit across the SMD3/SMD4 spins — SMD4 changes
only the PCB layout, re-routing the same nets onto 2 copper layers instead of
4 (see [§8, 2-layer PCB notes](#8-2-layer-pcb-notes)) and adding a microSD
card-detect connection SMD3 left unwired (see [§3](#3-gpio-pin-map)).

Reference designators below follow the current schematic: **U1** = the
ESP32-C6-DevKitC-1 carrier, **U3**/**U4** = the two AG1171 SLICs, **J8**/**J9**
= their RJ12 jacks, **J1** = the DC jack, **D1** = the power diode-OR diode,
**J5** = the line-1/microSD jumper header, **Card1** = the microSD socket.

## 2. Power tree

Two 5 V sources feed one board-level **+5 V** rail through a diode-OR, so
either input works alone and neither can back-feed the other:

```text
USB-C (DevKit's own port) ──► DevKit onboard 5V/3.3V reg ──► DevKit 3V3 pin
                                                                    │
J1 (DC jack) ──[D1, Schottky, cathode → +5V]──┐                    │
                                               ▼                    ▼
                                        board +5V rail ──────► ES8388, I²C
                                               │                pull-ups
                                               └──────────────► U3/U4 +VPWR (SLICs)
```

- **USB-C** power comes in through the DevKit module's own USB-C connector
  and is regulated by the DevKit's own onboard supply — nothing on the
  carrier PCB does that conversion.
- **DC jack (J1)** is an alternative 5 V input, isolated from USB-C by a
  series Schottky diode (**D1**, same SS14 class used elsewhere on this
  design), cathode toward +5 V. Whichever source is present (or higher)
  supplies the rail; the diode blocks reverse current into the other source.
- The diode drops roughly 0.3–0.5 V. If the DC supply needs to clear the
  DevKit onboard regulator's minimum input headroom after that drop, spec
  the DC brick a bit above 5 V (5.5–6 V) rather than exactly 5 V — check the
  specific DevKit module's onboard regulator datasheet for its minimum input.
- **No separate 3.3 V LDO on this board.** The carrier PCB's 3.3 V rail
  (ES8388 + its I²C pull-ups, and any other 3.3 V logic on the PCB) is drawn
  directly from the DevKit module's own 3.3 V output pin.
- The raw **+5 V** rail (before the DevKit's regulator) also feeds each
  AG1171's **+VPWR**, with local bulk capacitance at each SLIC.
- The DevKit module's own **EN/RESET** is left unconnected on the carrier —
  the module already has its own pull-up and reset button onboard.

## 3. GPIO pin map

Full current pin map (ESP32-C6-DevKitC-1, **U1**). This carries over
unchanged from the prior spin, plus one addition: **GPIO3**, previously a
spare pin brought to a header, now wires to the microSD card-detect switch.

| ESP32-C6 GPIO | Signal | Notes |
|--|--|--|
| GPIO0 | I²S **DIN** | from codec **ASDOUT** (capture) |
| GPIO1 | I²S **LRCK** | |
| GPIO2 | I²S **MCLK** | 12.288 MHz (256× 48 kHz) |
| GPIO3 | microSD **CD** (card detect) | **Card1** pin 9 → GPIO3 directly; internal weak pull-up (~45 kΩ), no external resistor. Normally-open switch shorts to GND when a card is seated: **LOW = card present, HIGH = empty.** |
| GPIO4 (MTMS, strap) | Line 0 **F/R** | direct to **U4** |
| GPIO5 (MTDI, strap) | Line 0 **RM** | direct to **U4** |
| GPIO6 | Line 0 **SHK** | via **R14/R15** divider, direct to **U4** |
| GPIO7 | Line 0 **PD** | via diode (diode-only, no pull-up — see [§5](#5-ag1171-slic--rj12-wiring)), direct to **U4** |
| GPIO8 (BOOT, strap) | — | unconnected, free |
| GPIO9 (BOOT, strap) | — | unconnected, free — this is the download-mode strap |
| GPIO10 | I²S **DOUT** | to codec **DSDIN** (playback) |
| GPIO11 | I²S **BCLK** | to codec **SCLK** |
| GPIO12 (USB D−) | DevKit **USB** port | native USB-Serial-JTAG — the board's flash/console connection; not free. **On the 30-pin clone module these carry I²S MCLK and microSD card-detect instead** — see §11 |
| GPIO13 (USB D+) | DevKit **USB** port | native USB-Serial-JTAG; not free. See §11 for the clone module |
| GPIO14 | — | not brought out on the DevKit's header (module packaging limit) |
| GPIO15 (JTAG, strap) | **BM64 MFB** (J2 pin 4) | **SMD5 change.** Bluetooth module's power button, with a 100 kΩ pull-down so it idles low while the ESP32 is in reset. Free if Bluetooth isn't fitted. Strapping pin, but it only selects the JTAG source and only when an eFuse that ships unburned is set |
| GPIO16 (U0TXD) | header **J3** pin 1 | UART TX, optional/spare |
| GPIO17 (U0RXD) | header **J4** pin 1 | UART RX, optional/spare |
| GPIO18 | SD **MISO** *or* Line 1 **F/R** | shared via jumper header **J5** |
| GPIO19 | SD **CLK** *or* Line 1 **SHK** | shared via **J5** |
| GPIO20 | SD **MOSI** *or* Line 1 **RM** | shared via **J5** |
| GPIO21 | SD **CS** *or* Line 1 **PD** | shared via **J5** |
| GPIO22 | I²C **SDA** | codec control, + 4.7 kΩ pull-up to 3.3 V |
| GPIO23 | I²C **SCL** | codec control, + 4.7 kΩ pull-up to 3.3 V |
| EN/RESET | — | not connected on the carrier — the DevKit module has its own onboard pull-up + reset button |
| GPIO24–30 | — | not usable — internal flash, not part of this module's usable pin set |

**Reference/silk numbering note:** two SLICs, three numbering conventions —
go by reference designator, not by reading a net name in isolation:

| Reference | Firmware line | RJ12 jack | GPIO group |
|--|--|--|--|
| **U4** | line 0 | **J9** | GPIO4/5/6/7 (fixed, always wired) |
| **U3** | line 1 | **J8** | GPIO18–21 (shared with microSD via **J5**) |

**Strapping pins** (GPIO4, 5, 8, 9, 15): fine to use as general I/O after
boot; what's wired to them just can't force the wrong level *during*
power-on/reset. GPIO4/5 drive the SLIC as ESP32 **outputs**, which is a safe
direction for a strapping pin. GPIO9 is the one that actually matters
(download-mode select) and is left unconnected on this board.

**GPIO12/13**: native USB (USB-Serial-JTAG), wired to the DevKit's own USB
port. That's how this board is flashed and how its console is reached
(`/dev/cu.usbmodem…`), so don't reuse them. They *can* be plain GPIOs in
silicon, but only by giving up native USB, and the ROM enables USB on them
(D+ pull-up included) at every reset regardless of firmware.

**GPIO24–30**: internal flash, never available. **GPIO14**: not led out on
this module's package.

### J5: line-1 SLIC vs. microSD (jumper-selected)

GPIO18–21 are wired to the microSD socket directly; line 1's SLIC control
signals (F/R, RM, PD, SHK) are brought out to a second row on header **J5**
(2×4, 0.1"). Bridging shunts across each pair on **J5** routes those four
GPIOs to line 1's SLIC instead of the microSD card:

| J5 pin | Signal | J5 pin | Signal |
|--|--|--|--|
| 1 | GPIO21 (SD CS) | 2 | Line 1 PD |
| 3 | GPIO20 (SD MOSI) | 4 | Line 1 RM |
| 5 | GPIO19 (SD CLK) | 6 | Line 1 SHK |
| 7 | GPIO18 (SD MISO) | 8 | Line 1 F/R |

Bridge 1-2, 3-4, 5-6, 7-8 to enable line 1 and disable the microSD socket;
leave all four open (no jumper block populated) to use the microSD socket
instead. Without a jumper installed, line 1 has no GPIO path at all — see
[§7, build variants](#7-build-variants) for how this pairs with which
hardware gets stuffed.

## 4. ES8388 codec

One stereo codec serves both phone lines over a single I²S bus: line 0 uses
the codec's left channel, line 1 uses the right channel — each with its own
dedicated ADC input and DAC output, so both lines' receive audio digitizes
simultaneously every I²S frame.

| Pin | Name | Connect to |
|-----|------|------------|
| **1** | **MCLK** | ESP32 **GPIO2** — 12.288 MHz from ESP32 I²S master |
| **2** | **DVDD** | +3.3 V + 100 nF → DGND |
| **3** | **PVDD** | +3.3 V + 100 nF → DGND |
| **4** | **DGND** | Digital ground |
| **5** | **SCLK** | ESP32 **GPIO11** (I²S BCLK) |
| **6** | **DSDIN** | ESP32 **GPIO10** (I²S DOUT — MCU → codec, playback) |
| **7** | **LRCK** | ESP32 **GPIO1** |
| **8** | **ASDOUT** | ESP32 **GPIO0** (I²S DIN — codec → MCU, capture) |
| **9** | NC | leave NC |
| **10** | **VREF** | 1 µF → AGND |
| **11** | **ROUT1** | line 1 DAC out → coupling cap → **U3** (SLIC) **VIN** |
| **12** | **LOUT1** | line 0 DAC out → coupling cap → **U4** (SLIC) **VIN** |
| **13** | **HPGND** | GND (output-driver return) |
| **14** | **ROUT2** | optional monitor tap (same DAC signal as ROUT1) |
| **15** | **LOUT2** | optional monitor tap (same DAC signal as LOUT1) |
| **16** | **HPVDD** | +3.3 V + 100 nF → HPGND |
| **17** | **AVDD** | +3.3 V + 100 nF + 10 µF bulk → AGND |
| **18** | **AGND** | Analog ground |
| **19** | **ADCVREF** | 1 µF → AGND |
| **20** | **VMID** | 1 µF → AGND |
| **21** | **RIN2** | unused on this design — leave NC or tie to AGND |
| **22** | **LIN2** | unused on this design — leave NC |
| **23** | **RIN1** | line 1 ADC in ← coupling cap + series resistor ← **U3** (SLIC) **VOUT** |
| **24** | **LIN1** | line 0 ADC in ← coupling cap + series resistor ← **U4** (SLIC) **VOUT** |
| **25** | NC | leave NC |
| **26** | **CE** | GND → I²C address 0x10 (7-bit) |
| **27** | **CDATA** | ESP32 **GPIO22** (SDA) + 4.7 kΩ pull-up → 3.3 V |
| **28** | **CCLK** | ESP32 **GPIO23** (SCL) + 4.7 kΩ pull-up → 3.3 V |
| **EP** | Exposed pad | GND — multiple vias under pad |

I²S: 48 kHz, 16-bit stereo, ESP32 as master, codec as slave — one LRCK frame
carries one line-0 sample and one line-1 sample. No on-chip crystal; MCLK
comes entirely from the ESP32 I²S master.

**Single-ended inputs only.** Each line's receive audio lands on its own
dedicated ADC pin (LIN1 for line 0, RIN1 for line 1) — LIN2/RIN2 are not
used for audio on this board.

### Grounding (DGND / AGND / HPGND)

All three ground pins, plus the exposed pad, tie to the same board **GND**
plane — they are not isolated nets on this PCB. The split exists inside the
package so digital switching noise doesn't ride on sensitive analog
references; on the PCB, route each bypass cap's ground terminal to the
nearest matching pin for the shortest loop, then stitch the pins together
at the codec.

```text
Board GND plane ──┬── DGND   ◄── 100nF from DVDD, PVDD
                  ├── AGND  ◄── 100nF + 10µF from AVDD; 1µF from VREF/VMID/ADCVREF
                  ├── HPGND ◄── 100nF from HPVDD
                  └── EP (under pad)
```

Any monitor header on LOUT2/ROUT2 returns to the same GND plane, at or near
HPGND — do not float a monitor jack on a separate ground island.

## 5. AG1171 SLIC & RJ12 wiring

Each SLIC (**U4** = line 0, **U3** = line 1) interfaces one RJ12 jack (**J9**
line 0, **J8** line 1) with the following support network. This is the
current, corrected wiring — earlier hand-soldered revisions had a SLIC
Vin/Vout swap and a mono-mic-capture bug; both are fixed as of this design,
so only the corrected circuit is described below.

| Signal | Wiring |
|--------|--------|
| **+VPWR** | Board **+5 V** rail, with local **470 µF** electrolytic bulk capacitance per SLIC plus a 100 nF bypass close to the SLIC's power pins |
| **PD** (power-down) | Driven from an ESP32 GPIO through a series diode only (1N4148W-class, cathode toward the PD pin) — **no pull-up or pull-down resistor**. Per the AG1171 datasheet, PD should never be pulled to a logic high; GPIO low = power-down, GPIO high (diode blocks, PD floats) = run |
| **SHK** (hook status) | AG1171 SHK is a ~3.3 V logic output with an internal 3.3 kΩ pull-up (datasheet: VOH 2.8 V min) — **not** 5 V, whichever rail the SLIC runs from. **SMD5 uses a 10 kΩ / 100 kΩ divider**, giving ~2.9 V off-hook — comfortably above the ESP32-C6's 2.475 V input-high threshold (0.75 × VDD). This fixes SMD4's as-built 10 kΩ / 15 kΩ divider, which landed at ~1.9 V and only worked on some pads by luck |
| **F/R** (ring cadence), **RM** (ringing mode) | Driven directly as 3.3 V ESP32 GPIO outputs — no level shifting needed, since these are ESP32-driven control inputs on the SLIC, not SLIC-driven signals |
| **VIN** (needs external drive) | Fed from the codec's DAC output for that line — **LOUT1 → U4 VIN** (line 0), **ROUT1 → U3 VIN** (line 1) — through a coupling capacitor |
| **VOUT** (SLIC-driven) | Feeds the codec's ADC input for that line — **U4 VOUT → LIN1** (line 0), **U3 VOUT → RIN1** (line 1) — through a coupling capacitor and series resistor |
| **Tip/Ring** | Through a 4-diode full-wave bridge (M7/1N4007-class) at each RJ12 jack, so either wiring polarity on the line still reaches the SLIC correctly, then a line TVS (BZT03C82 or P6KE82 class, ~82 V clamp) across tip/ring for surge protection |

```text
+5V ──► +VPWR (SLIC)      470µF + 100nF locally at each SLIC

ESP32 GPIO ── PD net ──►|──► PD pin      (diode only — cathode toward PD, no pull-up)

ESP32 GPIO ◄── SHK ── 10k ──┬── (from SLIC SHK pin, ~3.3 V logic)
                            15k   ← as built; should be 100k (see below)
                             │
                            GND

ESP32 GPIO ─────────────► F/R    (output — ring cadence)
ESP32 GPIO ─────────────► RM     (output — ringing mode)

Codec LOUT1/ROUT1 ──[coupling cap]──► SLIC VIN
SLIC VOUT ──[coupling cap + series R]──► Codec LIN1/RIN1

Tip/Ring ◄──► RJ12 jack
              │
              └── [M7 ×4 full-wave bridge] ── [line TVS across tip/ring]
```

**SHK divider (fixed on SMD5).** The 15 kΩ pull-down makes the
off-hook level ~1.9 V, below the ESP32-C6's guaranteed input-high
threshold of 2.475 V. It shows up as a line whose `hookmon` never sees the
handset lift while its audio path works fine; on the first board where this
was diagnosed, line 0 (GPIO6) happened to read 1.9 V as high and line 1
(GPIO19) didn't. Fix: **change the 15 kΩ to 100 kΩ** (≈2.9 V off-hook;
the 100 kΩ still gives a defined low when the SLIC is powered down), or on
an existing board simply **remove the 15 kΩ** (≈3.1–3.2 V; the 10 kΩ stays
in series as current protection). Both are safe: SHK can't exceed its own
~3.3 V internal pull-up, under the ESP32's 3.6 V absolute maximum.

**Which parts to lift:** the 15 kΩ pull-downs are **R14** (line 0) and
**R12** (line 1). The 10 kΩ series resistors — **R15** (line 0) and **R13**
(line 1) — stay put. Removing both 15 kΩ resistors is **confirmed working on
real hardware** (2026-09-27): a board where line 1 had never registered a
handset lift read both lines correctly afterwards, with no other change.

**Mounting:** the AG1171 is a through-hole SIL module — hand-soldered even
on this SMD board.

## 6. microSD (Card1)

Populated only on single-line builds (see [§7](#7-build-variants)); its SPI
pins share copper with line 1's SLIC and are routed through **J5**.

| Card1 pin | Function | Connects to |
|-----------|----------|-------------|
| 1 | DAT2 (RSV) | +3.3 V via 10 kΩ pull-up |
| 2 | CDDAT3 (CS) | GPIO21 |
| 3 | CMD (DI / MOSI) | GPIO20 |
| 4 | VDD | +3.3 V + 100 nF bypass |
| 5 | CLK (SCLK) | GPIO19 |
| 6 | VSS | GND |
| 7 | DAT0 (D0 / MISO) | GPIO18 |
| 8 | DAT1 (RSV) | +3.3 V via 10 kΩ pull-up |
| **9** | **CD (card detect)** | **GPIO3** — internal pull-up, LOW = card present |
| 10–13 | GND (frame/shell) | GND |

Card-detect (pin 9) is this board's one schematic addition over the prior
spin: it was previously left unconnected. Firmware checks it before
touching the SPI bus, so a missing card is reported cleanly instead of a
failed mount attempt.

## 7. Build variants

One PCB, two stuffing options, selected by which SLIC/jack/socket parts are
populated and whether **J5**'s jumpers are bridged:

| Build | Populate | Leave unpopulated | J5 jumpers | Firmware |
|-------|----------|--------------------|------------|----------|
| **A — Dual line** | **U4**/**J9** (line 0, always), **U3**/**J8** (line 1) | **Card1** (microSD) | **Bridged** (1-2, 3-4, 5-6, 7-8) — routes GPIO18–21 to line 1's SLIC | `CONFIG_BOARD_NUM_LINES=2`, `CONFIG_BOARD_MICROSD=0` |
| **B — Single line + microSD** | **U4**/**J9** (line 0 only), **Card1** | **U3**, **J8** (line 1) | **Open** (no jumper block) — routes GPIO18–21 to the microSD socket | `CONFIG_BOARD_NUM_LINES=1`, `CONFIG_BOARD_MICROSD=1` |

Line 0 (**U4**/**J9**) is wired to its own fixed GPIOs (4/5/6/7) and is
present on every build. Line 1 and microSD are mutually exclusive: whichever
one you want, populate its parts and set **J5** to match.

## 8. 2-layer PCB notes

This board routes on 2 copper layers (`F.Cu`/`B.Cu`) rather than 4. Standard
2-layer tradeoffs apply and are worth keeping in mind when reviewing or
modifying the layout:

- **No dedicated ground/power plane** — GND is a deliberately-poured zone on
  one side with stitching vias tying the two layers together, rather than a
  continuous internal plane. Worth confirming a continuous GND pour exists
  under the audio path (SLIC VIN/VOUT, ES8388 analog pins) specifically,
  since a broken return path there shows up as noise/crosstalk rather than a
  dead short.
- **Current-carrying nets** — the +5 V feed to the AG1171s' VPWR doesn't have
  a plane to spread heat or voltage drop across; size that trace with normal
  2-layer current-carrying practice rather than assuming a plane will help.
- **EMI/crosstalk** is generally higher on 2 layers than 4 for the same
  routing density, since there's no plane between signal layers to shield
  one from another. Not disqualifying for this design — control signals and
  audio-band I²S are not high-speed enough to be fussy about it — just worth
  a visual check on the copper pour rather than assuming it's a wash.

## 9. Board size & connectors

| Item | Detail |
|------|--------|
| Board size | ~100 × 50 mm, 2-layer |
| Phone lines | RJ12 ×1 (single-line build) or ×2 (dual-line build), **J9**/**J8** |
| Power | USB-C (via the DevKit module's own port) and DC jack (**J1**), diode-ORed |
| Storage | microSD, optional, single-line build only |
| Debug | UART header (**J3** TX / **J4** RX), optional |

## 10. Bluetooth companion header (J2)

**SMD5 addition.** An 8-pin board-to-board header links a BM64 Bluetooth
Classic audio carrier to the main board. The carrier is a separate PCB
(`Bluetooth/Bluetooth_board/`) and **has not been fabricated**.

| J2 pin | Net | Carrier side | Notes |
|--------|-----|--------------|-------|
| 1 | `+3V3_BM` | BM64 `BAT_IN` | From **U5**, a dedicated XC6206 LDO off +5 V — not the DevKit's 3.3 V rail |
| 2 | GPIO16 / U0TXD | BM64 `HCI_RXD` | ESP transmits |
| 3 | GPIO17 / U0RXD | BM64 `HCI_TXD` | ESP receives |
| 4 | `MFB` (GPIO15) | BM64 `MFB` | Power button / UART wake, 100 kΩ pull-down on the main board |
| 5 | via **C22** 1 µF ← `Line0Vin`'s SLIC `VOUT` | BM64 `MIC_P1` | Handset mic → Bluetooth |
| 6 | via **C24** 1 µF ← GND | BM64 `MIC_N1` | AC ground for the differential mic input |
| 7 | via **C23** 1 µF → **R16** 1 kΩ → `Line0Vin` | BM64 `AOHPL` | Bluetooth audio → earpiece. The 1 kΩ limits fault current into the SLIC |
| 8 | GND | GND | |

Audio never passes through firmware — both directions are analog, between
the BM64 and line 0's SLIC. The ESP32 only carries control over the UART.

**Orientation matters.** Pin 1 is +3.3 V and pin 8 is GND, so mating the two
boards rotated 180° shorts the LDO output straight to ground. Mark pin 1 on
both silkscreens and dry-fit before applying power.

**Bluetooth is SMD5-only.** On SMD4 it takes hand-wiring and board
modifications, which is why [`hardware-smd4.md`](hardware-smd4.md) doesn't
describe it.

## 11. MCU module variants

The footprint accepts two modules, and they are **not** pin-compatible:
pad positions 12/13 and 19/20 swap between vendors.

| Pad | Genuine ESP32-C6-DevKitC-1 | 30-pin AliExpress clone |
|-----|---------------------------|--------------------------|
| 12 | `GPIO2` (I²S MCLK) | `GPIO12` / USB D− |
| 13 | `GPIO3` (microSD card-detect) | `GPIO13` / USB D+ |
| 19 | `GPIO12` / USB D− | `GPIO2` |
| 20 | `GPIO13` / USB D+ | `GPIO3` |

With the genuine module everything is as SMD4. With the clone, MCLK and
card-detect land on its native-USB pins, so the firmware build must be
`CONFIG_BOARD_TYPE_SMD5_CHEAP` — it remaps those two and detaches the USB
PHY at boot. Consequences: no USB console after ~1 s of boot, a seated
microSD card blocks USB entirely, and the **J3**/**J4** spare-UART header
becomes unreachable.

A planned fix (two solder jumpers selecting pad 12/13 or 19/20 for those two
nets) would let both variants land on GPIO2/GPIO3 and retire all of that.
Full detail, including the jumper wiring and the symptoms of getting it
wrong, is in
[`../archive/hardware/ANALOG_REDIAL_SMD5.md`](../archive/hardware/ANALOG_REDIAL_SMD5.md).
