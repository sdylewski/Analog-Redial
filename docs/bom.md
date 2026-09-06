# Bill of Materials — SMD4 Board

Shopping / build checklist for the current production board (design
reference: [`hardware.md`](hardware.md); KiCad: `hardware/kicad/Rotary_phone_SMD4/`).

Most builders order **JLCPCB** for SMT passives and JLC-placeable parts, then
fill in the hand-solder table below for everything JLC won't place. Mark rows
done in the leading empty column (e.g. `x` or `☑`), or strike through the
component name.

Reference designators follow `docs/hardware.md`: **U1** = ESP32-C6-DevKitC-1
carrier, **U3**/**U4** = AG1171 SLICs, **J8**/**J9** = their RJ12 jacks,
**J1** = DC jack, **D1** = diode-OR diode, **J5** = line-1/microSD jumper
header, **Card1** = microSD socket. Passives whose SMD4-specific reference
designator isn't confirmed from the schematic are listed by function/value
instead of by ref — the part number and value are what matters for ordering.

---

## Build options

| Build | Lines | microSD | J5 jumpers | Omit from hand-solder table |
|-------|-------|---------|------------|------------------------------|
| **A — Dual line** | **J9** + **J8** | No | Bridged (1-2, 3-4, 5-6, 7-8) | **Card1** |
| **B — Single + SD** | **J9** only | **Card1** | Open (no shunts) | **U3**, **J8**, line-1 SLIC support parts |

Firmware: `CONFIG_BOARD_NUM_LINES` 2 (A) or 1 (B); `CONFIG_BOARD_MICROSD` 0 (A) or 1 (B).

---

## What's different from the SMD2 parts list

This board's MCU carrier is a complete **ESP32-C6-DevKitC-1** dev board, not
a soldered WROOM-1 module — so there is **no onboard 3.3 V LDO**, **no
onboard USB ESD chip**, and **no onboard USB-C receptacle** on this PCB; USB
power/programming goes through the DevKit module's own USB-C port. In their
place, this board adds a **DC jack** as an alternate 5 V input, diode-ORed
onto the same +5 V rail that feeds the SLICs. Everything not tied to the MCU
carrier or USB-C input — ES8388, AG1171 SLICs, RJ12 jacks, microSD, most
passives — carries over unchanged from the SMD2 parts list.

---

## Hand-solder BOM

### Carrier & modules

| | Qty | Component | DigiKey | AliExpress / JLC | Notes |
|---|-----|-----------|---------|-------------------|-------|
| | 1 | MCU carrier: **U1** ESP32-C6-DevKitC-1 (or equivalent ESP32-C6 dev board) | `[TODO: confirm part number/price]` | `[TODO: confirm part number/price]` | Complete dev board — own USB-C, reset button, onboard 3.3 V reg; header/socket-mounted, not soldered flat |
| | 1 | Audio codec: ES8388 (QFN-28) | [C365736 / LCSC (~$1–3)](https://www.lcsc.com/product-detail/C365736.html) | | Usually JLC-placed; hand-solder if DNP |
| | 1 | SLIC: **U4** AG1171 @ **J9** (line 0) | [5061-AG1171-ND ($8.50)](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) | | Always populated |
| | 1 | SLIC: **U3** AG1171 @ **J8** (line 1) | same as **U4** | | Build **A** only |
| | 1 | microSD socket: **Card1** TF-01A | [C91145 / LCSC (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | | Build **B** only; usually JLC-placed |

### Power & DC input

| | Qty | Component | DigiKey | AliExpress / JLC | Notes |
|---|-----|-----------|---------|-------------------|-------|
| | 1 | **J1** DC barrel jack | `[TODO: confirm part number/price]` | `[TODO: confirm part number/price]` | Alternate 5 V input, diode-ORed with USB-C; confirm DevKit's onboard regulator's minimum input headroom before specifying jack voltage (5.5–6 V DC brick recommended if the diode drop matters) |
| | 1 | **D1** SS14 Schottky (diode-OR) | [SS14-FDICT-ND (~$0.10)](https://www.digikey.com/en/products/detail/onsemi/SS14/) | [C2480 / LCSC (~$0.05)](https://www.lcsc.com/product-detail/C2480.html) | Cathode toward +5 V; blocks backfeed between DC jack and USB-C |
| | 1 | Line TVS (line 0, @ **U4**/**J9**) | [BZT03C82-TAP-ND (~$1)](https://www.digikey.com/en/products/detail/vishay-general-semiconductor-diodes-division/BZT03C82-TAP/7464977) | | DO-41 axial; alt **P6KE82** per Silvertel |
| | 1 | Line TVS (line 1, @ **U3**/**J8**) | same as above | | Build **A** only |

No dedicated 3.3 V LDO and no USB ESD IC on this board — both functions live
on the DevKit module itself.

### Connectors & headers

| | Qty | Component | DigiKey | AliExpress / JLC | Notes |
|---|-----|-----------|---------|-------------------|-------|
| | 1 | **J9** RJ12 horizontal | [609-1055-ND ($0.83)](https://www.digikey.com/en/products/detail/amphenol-icc/87180-062LF/1159085) **87180-062LF** | | Alt **54601-006** [609-54601-006R-ND](https://www.digikey.com/en/products/detail/amphenol-icc/54601-006/1158894) |
| | 1 | **J8** RJ12 horizontal | same as **J9** | | Build **A** only |
| | 1 | **J5** 2×4 (8-pin) 2.54 mm jumper header + 4× shunts | `[TODO: confirm part number/price]` | `[TODO: confirm part number/price]` | Bridge all 4 pairs for line 1; leave open for microSD — see `hardware.md` §3 |
| | 1 | **J3** 1×2 header, UART TX (optional) | [S1011EC-40-ND (~$0.50)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-02-UA/5828900) | | GPIO16 |
| | 1 | **J4** 1×2 header, UART RX (optional) | [S1011EC-40-ND (~$0.50)](https://www.digikey.com/en/products/detail/adafruit-industries-llc/PH1-02-UA/5828900) | | GPIO17 |

### Diodes

| | Qty | Component | DigiKey | AliExpress / JLC | Notes |
|---|-----|-----------|---------|-------------------|-------|
| | 1 | PD diode, line 0 (1N4148W, SOD-123) | [78-1N4148W-T-ND (~$0.10)](https://www.digikey.com/en/products/detail/stmicroelectronics/1N4148W/1014052) | | Diode-only PD wiring — no pull-up, see `hardware.md` §5 |
| | 1 | PD diode, line 1 | same as above | | Build **A** only |
| | 4 | Tip/ring bridge, line 0 (M7 / 1N4007, SMA) | | [C18199088 / LCSC (~$0.02 ea)](https://www.lcsc.com/product-detail/C18199088.html) | Full-wave bridge @ **J9** |
| | 4 | Tip/ring bridge, line 1 | same as above | | Build **A** only, @ **J8** |

### Capacitors

| | Qty | Component | DigiKey | AliExpress / JLC | Notes |
|---|-----|-----------|---------|-------------------|-------|
| | 1 | 470 µF 16 V radial, line 0 SLIC bulk | [1189-1146-ND ($0.24)](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) **16YXJ470M8X11.5** | | **U4** +VPWR bulk |
| | 1 | 470 µF 16 V radial, line 1 SLIC bulk | same as above | | Build **A** only, **U3** +VPWR |
| | 1 | 100 nF 0402, SLIC +5 V bypass (line 0) | | [C1525 (~$0.01)](https://www.lcsc.com/product-detail/C1525.html) | Local to **U4** |
| | 1 | 100 nF 0402, SLIC +5 V bypass (line 1) | | same as above | Build **A** only, local to **U3** |
| | 2 | 10 nF 0402, SLIC VIN coupling (per line) | | [C15195 (~$0.01)](https://www.lcsc.com/product-detail/C15195.html) | 1 per line, build **B** = qty 1 |
| | 2 | 100 nF 0402, SLIC VOUT coupling (per line) | | [C1525 (~$0.01)](https://www.lcsc.com/product-detail/C1525.html) | 1 per line, build **B** = qty 1 |

### Resistors

| | Qty | Component | DigiKey | AliExpress / JLC | Notes |
|---|-----|-----------|---------|-------------------|-------|
| | 2 | 10 kΩ + 15 kΩ 0402, SHK divider (line 0) | | [C25744 (10k)](https://www.lcsc.com/product-detail/C25744.html) / [C25756 (15k)](https://www.lcsc.com/product-detail/C25756.html) | **R14/R15** per `hardware.md` §3 |
| | 2 | 10 kΩ + 15 kΩ 0402, SHK divider (line 1) | | same as above | Build **A** only |
| | 1 | 10 Ω 0402, ES8388 AVDD → HPVDD (optional) | | [C25077](https://www.lcsc.com/product-detail/C25077.html) | Optional ripple isolation |

**Upload note:** the DigiKey links above are individual parts, not a
pre-built cart CSV — this board's carrier/DC-jack/jumper-header parts aren't
final yet (see `[TODO]` rows), so a bulk-cart export isn't generated until
those are picked.

---

## JLCPCB SMT assembly

Mark all hand-solder refs **DNP** on the JLC BOM. **Extended loading:** ~$3
per unique non-Basic LCSC part (ES8388; build B adds **Card1**).

### Modules & connectors

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | ES8388 | [C365736 (~$1)](https://www.lcsc.com/product-detail/C365736.html) | Extended |
| 1 | **Card1** TF-01A | [C91145 (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | Build B; Extended |

No JLC-placed USB-C receptacle, LDO, or USB ESD chip on this board — those
parts are gone (carrier handles USB/3.3 V/ESD on the DevKit module itself).

### Diodes

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **D1** SS14 (diode-OR, DC jack) | [C2480 (~$0.05)](https://www.lcsc.com/product-detail/C2480.html) | Basic; cathode → +5 V |
| 1–2 | PD diode(s), 1N4148W | [C81598 (~$0.01)](https://www.lcsc.com/product-detail/C81598.html) | Basic; 1 per populated SLIC |
| 4–8 | Tip/ring bridge, M7 (1N4007) | [C18199088 (~$0.02)](https://www.lcsc.com/product-detail/C18199088.html) | Basic; 4 per populated SLIC |

### Resistors (0402)

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 2–4 | 10 kΩ, SHK divider + microSD DAT pull-ups | [C25744](https://www.lcsc.com/product-detail/C25744.html) | 1–2 per SHK divider, 2 for microSD (build B) |
| 1–2 | 15 kΩ, SHK divider | [C25756](https://www.lcsc.com/product-detail/C25756.html) | 1 per populated SLIC |
| 2 | 4.7 kΩ, I²C pull-ups (SDA/SCL) | [C25900](https://www.lcsc.com/product-detail/C25900.html) | Always populated |
| 1 | 10 Ω, ES8388 AVDD → HPVDD (optional) | [C25077](https://www.lcsc.com/product-detail/C25077.html) | Optional |

### Capacitors

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 3 | 1 µF 0402, ES8388 VREF/ADCVREF/VMID | [C52923](https://www.lcsc.com/product-detail/C52923.html) | Always populated |
| 5 | 100 nF 0402, ES8388 DVDD/PVDD/AVDD/HPVDD | [C1525](https://www.lcsc.com/product-detail/C1525.html) | Always populated |
| 1 | 10 µF 0603, ES8388 AVDD bulk | [C15525](https://www.lcsc.com/product-detail/C15525.html) | Always populated |
| 1–2 | 10 nF 0402, SLIC VIN coupling | [C15195](https://www.lcsc.com/product-detail/C15195.html) | 1 per populated SLIC |
| 1–2 | 100 nF 0402, SLIC VOUT coupling | [C1525](https://www.lcsc.com/product-detail/C1525.html) | 1 per populated SLIC |
| 1–2 | 100 nF 0402, SLIC +5 V bypass | [C1525](https://www.lcsc.com/product-detail/C1525.html) | 1 per populated SLIC |

**470 µF SLIC bulk caps are hand-solder only** — THT electrolytic, not a JLC
SMT part; see the hand-solder capacitors table above.

### JLC order checklist

| | Step |
|---|------|
| | Generate Gerber + BOM + CPL (JLCPCB Tools) |
| | DNP all hand-solder refs (carrier module, DC jack, jumper header, SLICs, RJ12, TVS, 470 µF bulk) on the JLC BOM |
| | Extended: **C365736** (ES8388); build B adds **C91145** (Card1) |
| | Confirm build A vs. build B stuffing before generating CPL — line 1's SLIC support parts and **Card1** are mutually exclusive |

---

## Open items before ordering

- **ESP32-C6-DevKitC-1 (or equivalent) part number and price** — not yet
  pinned down; confirm the specific module/vendor and its onboard
  regulator's minimum 5 V input headroom (affects the DC jack voltage spec)
  before finalizing this line.
- **DC jack (J1) part number and price** — not yet specified in the design
  docs; pick a standard 5.5×2.1 mm (or similar) barrel jack rated for the
  board's 5 V/SLIC current draw.
- **J5 jumper header part number** — a generic 2×4, 0.1" pin header plus 4
  shunts will work mechanically; no specific part has been chosen yet.
