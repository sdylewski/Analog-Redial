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

**DigiKey subtotal, priced rows below — build A (dual line): ~$31.11 · build
B (single + SD): ~$19.99.** Excludes the DC jack (still unpicked) and
everything JLC assembles. Prices checked 2026-09-30, quantity 1.

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | MCU carrier: **U1** ESP32-C6-DevKitC-1 (or equivalent) | [DigiKey](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-DEVKITC-1-N8/17728861) $9.00 | [AliExpress](https://www.aliexpress.us/item/3256806563223929.html) $7.72 · [AliExpress](https://www.aliexpress.us/item/3256809110312291.html) $3.99 | DigiKey stocks the **N8** (8 MB) only — fine, but see *Flash size* below. AliExpress $7.72 = 32-pin **N16** (16 MB), the known-good shape. $3.99 = 30-pin clone: **needs trace cuts + 2 solder bridges, and is untested here** |
| | 1 | SLIC: **U4** AG1171 @ **J9** (line 0) | [DigiKey](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) $8.50 | [AliExpress](https://www.aliexpress.us/item/3256804758849823.html) $3.79 | Always populated. 14-SIP; DigiKey lead time 16 weeks |
| | 1 | SLIC: **U3** AG1171 @ **J8** (line 1) | same as **U4** | same as **U4** | Build **A** only |
| | 1 | **J1** DC barrel jack | `[TODO: confirm part number/price]` | | Alternate 5 V input, diode-ORed with USB-C; confirm the DevKit regulator's input headroom before picking a voltage (5.5–6 V brick if the diode drop matters) |
| | 1 | Line TVS (line 0, @ **U4**/**J9**) | [DigiKey](https://www.digikey.com/en/products/detail/vishay-general-semiconductor-diodes-division/BZT03C82-TAP/7464977) ~$1.00 | | DO-41 axial; alt **P6KE82** per Silvertel |
| | 1 | Line TVS (line 1, @ **U3**/**J8**) | same as above | | Build **A** only |
| | 1 | **J9** RJ12 horizontal, 6p2c | [DigiKey](https://www.digikey.com/en/products/detail/amphenol-cs-fci/87180-062LF/1001369) $1.07 | [AliExpress](https://www.aliexpress.us/item/3256810043215346.html) $3.38 / 10 pcs | **87180-062LF**. AliExpress: pick the **322** variant. Alt **54601-006** — [DigiKey](https://www.digikey.com/en/products/detail/amphenol-icc/54601-006/1158894) |
| | 1 | **J8** RJ12 horizontal, 6p2c | same as **J9** | same as **J9** | Build **A** only |
| | 1 | **J5** 2×4 (8-pin) 2.54 mm header + 4× shunts | [DigiKey](https://www.digikey.com/en/products/detail/same-sky-formerly-cui-devices/SSK03-MPH-254-B4/28772142) $0.13 | | **SSK03-MPH-254-B4**. Only needed for a 2nd line *or* the Bluetooth option (its programming header mates here) — a single-line board with microSD leaves J5 open and can skip it |
| | 1 | 470 µF 16 V radial, line 0 SLIC bulk | [DigiKey](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) $0.42 | | **16YXJ470M8X11.5**, **U4** +VPWR bulk (was $0.24 — DigiKey price rose) |
| | 1 | 470 µF 16 V radial, line 1 SLIC bulk | same as above | | Build **A** only, **U3** +VPWR |

**Buying the three big items from AliExpress instead** (carrier + 2× AG1171
+ RJ12) costs about **$18.68** against **$28.14** at DigiKey, and gets you
16 MB of flash rather than 8 MB — at the cost of longer shipping and no
returns. The SLICs are the bulk of the saving.

### Flash size: 16 MB vs 8 MB

**The firmware fits either way** — the app is ~1.5 MB against a 1.75 MB OTA
slot. What shrinks on an 8 MB part is room for audio. The current 16 MB
layout (`firmware/partitions.csv`) spends 1.75 MB × 2 on OTA slots, 6 MB on
recordings (`/assets`) and 2 MB on built-in clips (`/bundled`), and still
leaves ~4.5 MB unused.

An 8 MB part needs a re-cut: keep both OTA slots (3.5 MB), shrink `bundled`
to ~1.75 MB (it only holds ~1.55 MB today) and `storage` lands at ~2.5 MB
instead of 6 MB — so lower the recording cap from its current 4 MB to
match. Dropping OTA entirely would buy back 1.75 MB for recordings, at the
cost of web-UI firmware updates. Either is a `partitions.csv` edit, not a
code change.

---

## JLCPCB SMT assembly

Mark all hand-solder refs **DNP** on the JLC BOM. **Extended loading:** ~$3
per unique non-Basic LCSC part (ES8388; build B adds **Card1**).

**Approximate SMT parts total: ~$2 (build A) / ~$2 (build B), plus ~$3–$6 in
extended-part fees** — ES8388 always, **Card1** on build B, and **C10/C11**
unless you downsize them (see *Capacitors*). Resistors and 0402 ceramics are
fractions of a cent each and aren't itemised below.

### Modules & connectors

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | ES8388 | [C365736 (~$1)](https://www.lcsc.com/product-detail/C365736.html) | Extended |
| 1 | **Card1** TF-01A | [C91145 (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | Build B; Extended |

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
| 2 | **C10/C11** 100 µF 1206, LOUT2/ROUT2 → **J7** monitor header | [C13585 (~$0.10)](https://www.lcsc.com/product-detail/C13585.html) | **Extended** — downsize to 1 µF 0402 unless you drive headphones from J7, see below |

**C10/C11 can almost certainly be 1 µF 0402 instead.** They AC-couple the
ES8388's second output pair (`LOUT2`/`ROUT2`, which the firmware just
mirrors from the earpiece volume) to **J7**, the 3-pin monitor header, each
with a 47 kΩ bleed to GND. 100 µF is sized for driving 16–32 Ω headphones
directly — into 32 Ω, 1 µF would put the high-pass corner near 5 kHz and
gut the audio. But into anything high-impedance — a scope probe, a line
input, a powered speaker — the 47 kΩ bleed dominates and **1 µF already
gives a ~3 Hz corner** (~19 Hz into a 10 kΩ line input), far below the
300–3400 Hz phone band. So unless you plan to plug headphones straight into
J7, swap to the 1 µF 0402 already on the reel: it drops an Extended part
(~$3 of the JLC fee) and two 1206 footprints. Keep ≥47 µF only for the
headphone case.

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
