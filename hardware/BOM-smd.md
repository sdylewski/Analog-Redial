# BOM — SMD board (`Rotary_phone_SMD2`)

**KiCad:** [`kicad/Rotary_phone_SMD2/`](kicad/Rotary_phone_SMD2/) · **Design:** [`ROTARY_PHONE_SMD.md`](ROTARY_PHONE_SMD.md) · **DigiKey CSV:** [`bom-digikey-hand-solder.csv`](kicad/Rotary_phone_SMD2/bom-digikey-hand-solder.csv) · **Cart export:** [`2026-07-02T230626.csv`](2026-07-02T230626.csv)

Most builders order **JLCPCB** for SMT passives, then fill in the hand-solder table below. **DigiKey** links/prices match [`2026-07-02T230626.csv`](2026-07-02T230626.csv) where listed.

Mark rows done in the leading empty column (e.g. `x` or `☑`), or strike through the component name.

---

## Build options

| Build | Lines | microSD | Omit from hand-solder table |
|-------|-------|---------|----------------------------|
| **A — Dual line** | **J6** + **J7** | No | **Card1** |
| **B — Single + SD** | **J6** only | **Card1** | **U6**, **J7**, **C21**, **D4**, **D14**, **R16–R19** (line-2 bridge **D9–D12** are JLC only) |

Firmware: `CONFIG_BOARD_NUM_LINES` 2 (A) or 1 (B); `CONFIG_BOARD_MICROSD` 0 (A) or 1 (B).

---

## Hand-solder BOM

### Modules & ICs

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | Microcontroller: **U2** ESP32-C6-WROOM-1-N16 | [ESP32-C6-WROOM-1-N16-ND (~$5)](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-WROOM-1-N16/14569322) | | Castellated; hot-air / reflow |
| | 1 | Audio codec: **U4** ES8388 (QFN-28) | [C365736 / LCSC (~$1–3)](https://www.lcsc.com/product-detail/C365736.html) | | Usually JLC-placed; hand if DNP |
| | 1 | SLIC: **U5** AG1171 @ **J6** | [5061-AG1171-ND ($8.50)](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) | | Line 1 |
| | 1 | SLIC: **U6** AG1171 @ **J7** | same as **U5** | | Build **A** only; line 2 |
| | 1 | microSD socket: **Card1** TF-01A | [C91145 / LCSC (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | | Build **B** only; usually JLC-placed |

### Regulators & protection

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | Regulator (3.3 V LDO): **U1** AP2112K-3.3 (SOT-23-5) | [AP2112K-3.3TRG1DICT-ND ($0.17)](https://www.digikey.com/en/products/detail/diodes-incorporated/AP2112K-3-3TRG1/4470746) | | JLC DNP; alt ME6211C33M5G-N |
| | 1 | USB ESD protection: **U3** USBLC6-2SC6 | [USBLC6-2SC6CT-ND (~$0.60)](https://www.digikey.com/en/products/detail/umw/USBLC6-2SC6/16705896) | | JLC DNP |
| | 1 | Line TVS: **D13** BZT03C82 or P6KE82 | [BZT03C82-TAP-ND (~$1)](https://www.digikey.com/en/products/detail/vishay-general-semiconductor-diodes-division/BZT03C82-TAP/7464977) | | @ **J6** / **U5**; DO-41 axial; alt **P6KE82** per Silvertel |
| | 1 | Line TVS: **D14** BZT03C82 or P6KE82 | same as **D13** | | Build **A**; @ **J7** |

### Connectors & headers

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

### Diodes

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **D3** 1N4148W (SOD-123) | [78-1N4148W-T-ND (~$0.10)](https://www.digikey.com/en/products/detail/stmicroelectronics/1N4148W/1014052) | | **U5** PD; usually JLC |
| | 1 | **D4** 1N4148W | [78-1N4148W-T-ND (~$0.10)](https://www.digikey.com/en/products/detail/stmicroelectronics/1N4148W/1014052) | | Build **A**; **U6** PD |

### Capacitors

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **C20** 470 µF 16 V radial | [1189-1146-ND ($0.24)](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) **16YXJ470M8X11.5** | | **U5** +VPWR bulk |
| | 1 | **C21** 470 µF 16 V radial | same as **C20** | | Build **A**; **U6** +VPWR |

Upload [`bom-digikey-hand-solder.csv`](kicad/Rotary_phone_SMD2/bom-digikey-hand-solder.csv) to [DigiKey myLists](https://www.digikey.com/en/mylists/) for bulk DigiKey ordering.

**Typical hand-solder kit (DigiKey, build A):** ~$20–25 per board (+ WROOM, USBLC6, TVS, headers — not in cart CSV), excluding shipping.

---

## DigiKey cart reference (2026-07-02)

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

## JLCPCB SMT assembly

Mark all hand-solder refs **DNP** on the JLC BOM. **Extended loading:** ~$3 per unique non-Basic LCSC (**USB1**, **U4**; build B add **Card1**).

JLC parts use **LCSC** links (not DigiKey). Typical total for 5 assembled boards: ~$35–58.

### Modules & connectors

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **U4** ES8388 | [C365736 (~$1)](https://www.lcsc.com/product-detail/C365736.html) | Extended |
| 1 | **USB1** USB-C 16P | [C2765186 (~$0.25)](https://www.lcsc.com/product-detail/C2765186.html) | Extended |
| 1 | **Card1** TF-01A | [C91145 (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | Build B; Extended |
| 1 | **U1** AP2112K-3.3 | [C82942 (~$0.15)](https://www.lcsc.com/product-detail/C82942.html) | **DNP** — hand |
| 1 | **U3** USBLC6 | [C7519 (~$0.25)](https://www.lcsc.com/product-detail/C7519.html) | **DNP** — hand |

### Diodes

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **D1** SS14 | [C2480 (~$0.05)](https://www.lcsc.com/product-detail/C2480.html) | Basic; VBUS → +5 V |
| 1 | **D2** LED 1206 red | [C2286 (~$0.01)](https://www.lcsc.com/product-detail/C2286.html) | Basic; GPIO11 |
| 1 | **D3** 1N4148W | [C81598 (~$0.01)](https://www.lcsc.com/product-detail/C81598.html) | Basic |
| 1 | **D4** 1N4148W | [C81598 (~$0.01)](https://www.lcsc.com/product-detail/C81598.html) | Build A |
| 4 | **D5–D8** M7 (1N4007) | [C18199088 (~$0.02)](https://www.lcsc.com/product-detail/C18199088.html) | Bridge @ **J6** |
| 4 | **D9–D12** M7 | [C18199088 (~$0.02)](https://www.lcsc.com/product-detail/C18199088.html) | Build A @ **J7** |

### Resistors (0402)

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

### Capacitors

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

### JLC order checklist

| | Step |
|---|------|
| | Generate Gerber + BOM + CPL (JLCPCB Tools) |
| | DNP all hand-solder refs on JLC BOM |
| | Extended: **C2765186**, **C365736**; build B add **C91145** |
| | Verify **D2** LED polarity in placement preview |

---

## Revision

| Date | Notes |
|------|-------|
| 2026-07 | DigiKey prices from `2026-07-02T230626.csv` |
| 2026-06 | Table format: Qty, Component, DigiKey, AliExpress, Notes |
