# BOM — Through-hole board (`rotary_phone_gateway`)

**Hand-solder / through-hole** PCB: ESP32-C6 + **WM8731** + **AG1170-S5** or **AG1171** ×2 (same SIL footprint). Axial / radial passives unless noted.

**KiCad:** [`hardware/kicad/Old/rotary_phone_gateway/`](../../../hardware/kicad/Old/rotary_phone_gateway/) · **Codec:** [`WM8731.md`](WM8731.md) · **GPIO:** [`../../../firmware/components/board/include/board_pins.h`](../../../firmware/components/board/include/board_pins.h)

**DigiKey pricing / part numbers:** `2026-07-02T230626.csv` — referenced here historically; this cart export was never actually committed to the repo, so the link below is dead. Prices in this doc are frozen as of that export where listed.

Put link and price in the same **DigiKey** / **AliExpress** cell (example: `[part (~$0.10)](url)`). Mark the leading empty column when ordered.

---

## Shopping BOM

### Modules & ICs

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
|x | 1 | Microcontroller: **U1** ESP32-C6-DevKitM-1 | — | | Espressif distributors (~$8–12); plugs into headers; need 16M version?!!? |
| x| 1 | Audio codec: **U2** WM8731 (SSOP-28) | [1327-WM8731SEDSVTR-ND (~$4)](https://www.digikey.com/en/products/detail/cirrus-logic-inc/WM8731SEDS-V/829701) | | Hand solder |
| o| 1 | Regulator (codec core): **U3** MCP1700-1.8 (SOT-89) | [MCP1700T-1802E/TTCT-ND](https://www.digikey.com/en/products/detail/microchip-technology/MCP1700T-1802E-T/418892) | | 1.8 V for WM8731 DVDD; **SOT-89** on PCB (not SOT-23 from cart) |
|x | 2 | SLIC: **U4**, **U5** AG1170-S5 or AG1171 | [5061-AG1171-ND ($8.50)](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) | [AG1170 (~$9 ea)](https://silvertel.com/ag1170/) | Same 14-pin SIL; **AG1171** on DigiKey. [Mouser AG1170-S5](https://www.mouser.com/ProductDetail/Silvertel/AG1170-S5) |

### Semiconductors (discrete)

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| o| 2 | **Q1**, **Q2** BC846 NPN (SOT-23) | [5399-BC846ACT-ND ($0.05)](https://www.digikey.com/en/products/detail/onsemi/BC846A/110606) | | **BC846A**; alt BC846B; PD drive |

### Connectors & headers

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| o| 2 | **J1**, **J2** RJ12 horizontal | [609-1055-ND ($0.83)](https://www.digikey.com/en/products/detail/amphenol-icc/87180-062LF/1159085) **87180-062LF** | | Alt **54601-006** [609-54601-006R-ND](https://www.digikey.com/en/products/detail/amphenol-icc/54601-006/1158894) |
| | 2 | **J4**, **J5** 1×2 header 2.54 mm | [S1011EC-40-ND (~$0.50)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-02-UA/5828900) | | Line control to DevKit |
| | 1 | **J7** 1×3 header 2.54 mm (optional) | [S1012EC-40-ND (~$0.55)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-03-UA/5828902) | | Audio monitor |
| | 1 | **J3** 1×2 test header (optional) | | | Cut from 2.54 mm strip |
| | 1 | Pin header strip 2.54 mm | | | Cut for **J3–J7**; ~$1–3 |

### Diodes

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| o| 8 | **D1–D7**, **D9** 1N4004 | [1N4004-E3/54GI-ND (~$0.10)](https://www.digikey.com/en/products/detail/vishay-semiconductor-dioces-division/1N4004-E3-54GI/14374445) | | Tip/ring bridges |
|o | 2 | Line TVS: **D8**, **D10** BZT03C82 or P6KE82 | [BZT03C82-TAP-ND (~$1)](https://www.digikey.com/en/products/detail/vishay-general-semiconductor-diodes-division/BZT03C82-TAP/7464977) | | DO-41 axial; alt **P6KE82** per Silvertel |

### Resistors (axial ¼ W)

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| o| 2 | **R1**, **R2** 4.7 kΩ | [CF14JT4K70CT-ND ($0.02)](https://www.digikey.com/en/products/detail/stackpole-electronics-inc/CF14JT4K70/1741376) | | I²C pull-ups |
| o| 2 | **R3**, **R5** 15 kΩ | [CF14JT15K0CT-ND ($0.02)](https://www.digikey.com/en/products/detail/stackpole-electronics-inc/CF14JT15K0/1741412) | | SHK divider → GND |
| x| 4 | **R4**, **R6**, **R9**, **R10** 10 kΩ | [CF14JT100RCT-ND (~$0.10)](https://www.digikey.com/en/products/detail/stackpole-electronics-inc/CF14JT100R/1741400) | | SHK + codec bias |
|x | 2 | **R7**, **R8** 10 Ω | [CF14JT10R0CT-ND (~$0.10)](https://www.digikey.com/en/products/detail/stackpole-electronics-inc/CF14JT10R0/1741394) | | WM8731 HPVDD |
|x | 2 | **R11**, **R12** 1 kΩ | [CF14JT1K00CT-ND (~$0.10)](https://www.digikey.com/en/products/detail/stackpole-electronics-inc/CF14JT1K00/1741404) | | PD base |

### Capacitors

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| o| 12 | **C1**, **C3–C5**, **C9–C12**, **C15–C18** 100 nF disc | [478-1393-ND (~$0.15)](https://www.digikey.com/en/products/detail/kemet/C320C104M5R5TA7300/3089153) | | |
| x| 2 | **C2**, **C6** 10 µF radial | [1186-1588-ND (~$0.20)](https://www.digikey.com/en/products/detail/rubycon/10YXF10M/3089266) | | |
| x| 2 | **C7**, **C8** 100 µF 6.3 V radial | [1186-1070-ND (~$0.25)](https://www.digikey.com/en/products/detail/rubycon/6-3YXF100M6-3X11/3089248) | | |
| x| 2 | **C13**, **C14** 220 µF 10 V low-ESR | [16YXH220MZX11.5CT-ND (~$0.35)](https://www.digikey.com/en/products/detail/rubycon/16YXH220MZX11-5/3134106) | | SLIC bulk |
| x| 2 | **C19**, **C20** 220 µF | | | Verify footprint; use radial electrolytic |

### PCB

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | PCB `rotary_phone_gateway` | | | Fab from [`gerbers/`](../../../hardware/kicad/Old/rotary_phone_gateway/gerbers/); ~$3–5/board @ qty 5 |

---

## Approximate cost (one board)

| Category | ≈ USD |
|----------|-------|
| DigiKey passives + connectors + TVS | ~$15–20 |
| 2× SLIC (AG1171 @ DigiKey) | ~$17 |
| DevKitM-1 + WM8731 + MCP1700 | ~$13–17 |
| PCB (5× shared) | ~$3–5 per board |
| **Typical total** | **~$50–60** |

---

## Firmware

Targets this board today (`board_pins.h`, **WM8731**). SMD board: [`../../ESP32_PORT_PLAN.md`](../../ESP32_PORT_PLAN.md).

---

## Revision

| Date | Notes |
|------|-------|
| 2026-07 | DigiKey prices from `2026-07-02T230626.csv` |
| 2026-06 | Removed Pi / Build P / Build E variants; table format |
