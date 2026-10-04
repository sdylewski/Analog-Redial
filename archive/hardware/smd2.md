# Analog Redial v2 (SMD2) — reference

The first surface-mount board: ESP32-C6-WROOM-1 module, ES8388 codec, one or
two AG1171 SLICs, optional microSD. Three were built, all hand-soldered and
reworked as below. KiCad project: `hardware/kicad/Analog Redial v2/`;
firmware `CONFIG_BOARD_TYPE_SMD2`, `board_pins_smd.h`, build dir
`build.smd2/`.

## Rework checklist for a bare board

Every SMD2 needs all of these.

1. **USB D+/D− swapped** between U3 (USBLC6) and the module. Cut **both**
   traces between U3 and module pins 13/14, then cross-wire with two short
   wires kept next to each other (it's 12 Mbit/s USB): U3's D+ side → pin 14
   (GPIO13), D− side → pin 13 (GPIO12). Without this the board never shows
   up on USB. If the wires go on but the traces aren't cut, D+ and D− are
   shorted: both read ~2.9 V and the host sees nothing. Healthy, plugged in:
   D+ ≈ 3 V, D− ≈ 0 V.
2. **SLIC Vin/Vout swapped** at the coupling caps, both lines. Cut each
   cap-to-SLIC trace and cross them (wire length doesn't matter at audio
   rates; the AG1171 can stay in place):
   - Line 0 (U6): C16 pin 2 → U6 pin 10, C17 pin 2 → U6 pin 9.
   - Line 1 (U5): C19 pin 2 → U5 pin 10, C18 pin 2 → U5 pin 9.
   Without it no audio reaches the phone line.
3. **Line 1 mic onto its own ADC channel.** Remove C24 (100 nF, RIN1 → GND,
   next to U4) and wire a new 100 nF from U5's Vout to C24's RIN1 pad.
   Firmware fixes `LINSEL`/`RINSEL` = 00, so line 0 is LIN1 and line 1 is
   RIN1, captured simultaneously. Without it line 1 is never heard in a
   call.
4. **DTMF level divider** on each SLIC → codec input. Real touch-tone peaks
   (~2.5 Vpp at Vout) overrun the ES8388's ~1 Vrms full scale. Desolder the
   SMD coupling cap, put a **10 kΩ** in its pad, wire a leaded **100 nF** from
   the SLIC's Vout to the 10 kΩ's other end, and add **8.2 kΩ** from the
   codec pin to GND. Order matters: cap → 10 kΩ → codec pin, with the
   8.2 kΩ at the codec pin. Result: DTMF peaks ~half of full scale, all 12
   keys detected.
5. **U1 (LDO) EN, pin 3, tied to VIN** (pin 1). Floating, the 3.3 V rail may
   not start.
6. **SHK divider** (10 kΩ / 15 kΩ) assumes a 5 V SHK swing; the AG1171's is
   ~3.3 V, so off-hook can read ~1.9 V, below the C6's 2.475 V input-high.
   If a line doesn't see the handset lift, remove that line's 15 kΩ (R16 or
   R18).

No BOOT (J2) jumper is needed to flash over USB: the chip's USB port shows
up even with blank flash and esptool enters download mode itself. J2 is for
flashing through the UART pins (J3/J4).

Firmware keeps the ES8388 mic PGA at **0 dB**: the 10 kΩ/8.2 kΩ divider is
sized against the codec's ~20 kΩ input impedance, which drops at higher PGA
gain and would attenuate speech.

## Board

| | |
|--|--|
| MCU | ESP32-C6-WROOM-1-N16 (castellated, hand-soldered), PCB antenna — keep ~15 mm clear |
| Codec | ES8388 QFN-28 (JLC-placed), I²C 0x10 (CE → GND), I²S slave |
| SLICs | AG1171 ×1–2, 14-pin SIL, hand-soldered |
| Power | USB-C only: VBUS → D1 SS14 → +5 V → U1 AP2112K-3.3 → +3.3 V; +5 V also feeds the SLICs' +VPWR |
| Passives | 0402 (10 µF 0603, 22 µF 0805), JLC-placed |

### Designators

| Ref | Part | Role |
|--|--|--|
| U1 | AP2112K-3.3 (alt ME6211C33M5G-N) | 3.3 V LDO, EN tied to VIN |
| U2 | ESP32-C6-WROOM-1-N16 | MCU |
| U3 | USBLC6-2SC6 | USB D+/D− ESD |
| U4 | ES8388 | codec |
| U5 | AG1171 | firmware **line 1**, RJ12 **J6** (silk "Line 1") |
| U6 | AG1171 | firmware **line 0**, RJ12 **J7** (silk "Line 2") — not fitted on the microSD build |
| USB1 | TYPE-C 16PIN 2MD(073) | USB-C |
| Card1 | HRO TF-01A | microSD — microSD build only |
| J2 | 1×2 header | GPIO9 boot strap + GND |
| J3 / J4 | 1×2 headers | UART TX (GPIO16) / RX (GPIO17) + GND |
| J5 | 1×3 header | audio monitor L / R / GND (LOUT2/ROUT2) |
| J8 | 2-pin 5 mm terminal | optional bench +5 V |
| D1 | SS14 | VBUS → +5 V (cathode to +5 V) |
| D2 + R6 | red 1206 LED + 220 Ω | status LED, GPIO11, active high |
| D3 / D4 | 1N4148W | U5 / U6 PD (cathode to PD) |
| D5–D8 / D9–D12 | M7 (1N4007) ×4 | tip/ring bridge, J6 / J7 |
| D13 / D14 | BZT03C82 or P6KE82 | line TVS, J6 / J7 |
| R1 + C1 | 10 kΩ + 1 µF | module EN pull-up / RC delay |
| R2, R3 | 10 kΩ | microSD DAT2 / DAT1 pull-ups |
| R4, R5 | 5.1 kΩ | USB-C CC1 / CC2 to GND |
| R7–R10 | 0 Ω | GPIO18–21 to line 1's SLIC **or** microSD — fit one side |
| R11, R12 | 4.7 kΩ | I²C pull-ups |
| R13 | 10 Ω | AVDD → HPVDD (optional) |
| R14, R15 | 47 kΩ | J5 monitor bleed |
| R17 + R16 / R19 + R18 | 10 kΩ + 15 kΩ | U5 / U6 SHK dividers |
| C2 / C3 / C4, C5 | 10 µF / 22 µF / 100 nF | +5 V / +3.3 V bulk / bypass |
| C6, C7, C8 | 1 µF | codec VREF / ADCVREF / VMID |
| C9, C10, C12, C13 / C11 | 100 nF / 10 µF | codec DVDD, PVDD, AVDD, HPVDD / AVDD bulk |
| C14, C15 | 100 µF | J5 monitor coupling |
| C16–C19 | 10 nF / 100 nF | SLIC ↔ codec coupling (see rework 2 and 4) |
| C20, C21 | 470 µF 16 V radial | SLIC +VPWR bulk |
| C22, C23 | 100 nF | +5 V bypass at each SLIC |
| C24, C25 | 100 nF | RIN1 / RIN2 (C24 replaced by rework 3) |

Net names: `Line1_*` go to U6/J7 (firmware line 0), `Line2_*` to U5/J6
(firmware line 1).

## Pin map (`board_pins_smd.h`)

| GPIO | Module pin | Signal |
|--|--|--|
| 0 | 8 | I²S MCLK (12.288 MHz) → codec pin 1 |
| 4 | 4 | I²S BCLK → codec SCLK (5) |
| 5 | 5 | I²S LRCK → codec LRCK (7) |
| 6 | 6 | I²S DOUT → codec DSDIN (6) |
| 7 | 7 | I²S DIN ← codec ASDOUT (8) |
| 22 / 23 | 20 / 21 | I²C SDA / SCL → codec CDATA (27) / CCLK (28) |
| 1 | 9 | line 0 SHK (via R19/R18) |
| 2 | 27 | line 0 F/R |
| 3 | 26 | line 0 RM |
| 10 | 11 | line 0 PD (via D4) |
| 18 | 16 | line 1 PD **or** SD CS |
| 19 | 17 | line 1 SHK **or** SD CLK |
| 20 | 18 | line 1 RM **or** SD MOSI |
| 21 | 19 | line 1 F/R **or** SD MISO |
| 11 | 12 | status LED |
| 9 | 15 | J2 boot strap (download mode when shorted at reset) |
| 12 / 13 | 13 / 14 | native USB D− / D+ (through U3) |
| 16 / 17 | 25 / 24 | UART0 TX / RX on J3 / J4 |

Strapping pins 8, 9, 15: leave unloaded at reset. GPIO14 isn't brought out
on the WROOM-1.

## Build options

GPIO18–21 serve line 1's SLIC **or** the microSD socket, chosen by which
side of R7–R10 is fitted:

| Build | Fit | Leave off | Firmware |
|--|--|--|--|
| Two lines | U5, U6, J6, J7, both SLIC networks, R7–R10 on the SLIC side | Card1 | two lines, no microSD |
| One line + microSD | U5, J6, Card1, R2/R3, R7–R10 on the SD side | U6, J7, C21, D4, D9–D12, D14, R18/R19 | one line + microSD |

Set the matching choice in Settings → Hardware. Firmware can't detect which
is fitted: those pins float without a driver, and a populated SLIC and a
microSD board read the same at boot.

## Audio path

```text
J6/J7 (RJ12) ◄─► U5/U6 (AG1171) ◄─► U4 (ES8388) ◄─I²S/I²C─► U2 (ESP32-C6)
```

After rework: each codec DAC output (LOUT1 line 0, ROUT1 line 1) drives its
SLIC's Vin through a 100 nF cap; each SLIC's Vout feeds its own ADC input
(LIN1 line 0, RIN1 line 1) through the DTMF divider. LOUT2/ROUT2 mirror the
DAC outputs to J5 for monitoring (enable in codec register 4).

ES8388 grounds (DGND 4, AGND 18, HPGND 13, exposed pad) all tie to the one
board GND at the codec; route each bypass cap to its nearest matching pin.

**AEC echo path** (`echoprobe`, 5-run repeats): bulk delay ~20–23 ms plus
~10–13 ms tail, ~31–36 ms total, in every handset position — inside the
canceller's 320-tap / 40 ms filter. Per-board delays are stored in NVS
(`aec delay`); measured: DualPhone 23 ms, MaxPhone 20 ms.

## microSD (Card1, TF-01A push-push)

| Card1 pin | Signal |
|--|--|
| 1 DAT2 | 10 kΩ (R2) to 3.3 V |
| 2 DAT3/CS | GPIO18 |
| 3 CMD | GPIO20 (MOSI) |
| 4 VDD | 3.3 V + 100 nF |
| 5 CLK | GPIO19 |
| 6 VSS, 10–13 frame | GND |
| 7 DAT0 | GPIO21 (MISO) |
| 8 DAT1 | 10 kΩ (R3) to 3.3 V |
| 9 CD | not connected (firmware treats a failed mount as no card) |

Alternative sockets with different footprints: Molex 104031-0811 (friction,
stock KiCad footprint), Molex 503398-1892 (push-push).

## BOM

**JLC-placed** (passives Basic; ~$3 loading fee per unique Extended part):
all R/C, D1 SS14 (C2480), D2 LED (C2286), D3/D4 1N4148W (C81598), M7 bridges
(C18199088), USB1 (C2765186, Extended), U4 ES8388 (C365736, Extended),
Card1 TF-01A (C91145, Extended, microSD build). Mark the hand-solder parts
DNP on the JLC BOM.

**Hand-soldered:**

| Qty | Part | Source |
|--|--|--|
| 1 | U2 ESP32-C6-WROOM-1-N16 | [DigiKey](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-WROOM-1-N16/14569322) |
| 1–2 | U5/U6 AG1171 | [DigiKey 5061-AG1171-ND](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) |
| 1 | U1 AP2112K-3.3TRG1 | [DigiKey](https://www.digikey.com/en/products/detail/diodes-incorporated/AP2112K-3-3TRG1/4470746) |
| 1 | U3 USBLC6-2SC6 | [DigiKey](https://www.digikey.com/en/products/detail/umw/USBLC6-2SC6/16705896) |
| 1–2 | D13/D14 BZT03C82 (or P6KE82) | [DigiKey](https://www.digikey.com/en/products/detail/vishay-general-semiconductor-diodes-division/BZT03C82-TAP/7464977) |
| 1–2 | J6/J7 RJ12 87180-062LF (or 54601-006) | [DigiKey](https://www.digikey.com/en/products/detail/amphenol-icc/87180-062LF/1159085) |
| 1–2 | C20/C21 470 µF 16 V Rubycon 16YXJ470M8X11.5 | [DigiKey](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) |
| — | J2 1×2 header + shunt; optional J3/J4 (1×2), J5 (1×3), J8 terminal | |
| per line | DTMF rework: 10 kΩ, 8.2 kΩ, leaded 100 nF | |

A DigiKey upload list for the hand-solder parts:
[`bom-digikey-hand-solder.csv`](../../hardware/kicad/Analog%20Redial%20v2/bom-digikey-hand-solder.csv)
(DigiKey myLists → Upload from spreadsheet). The ESP32 footprint is vendored
in `hardware/kicad/Analog Redial v2/Scott.pretty/`.

## See also

- [`smd4.md`](smd4.md) — the current board.
- [`BOM.md`](BOM.md) — parts-list index.
