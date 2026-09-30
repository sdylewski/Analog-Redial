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

| Build | Lines | microSD | J5 jumpers | Hand-solder BOM |
|-------|-------|---------|------------|------------------|
| **A — Dual line** | **J9** + **J8** | No | Bridged (1-2, 3-4, 5-6, 7-8) | Base board **+ second line** |
| **B — Single + SD** | **J9** only | **Card1** | Open (no shunts) | Base board only |

Either build can add the Bluetooth companion.

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

Built up by option: everyone needs the base board, then add one table per
option. DigiKey prices, quantity 1, checked 2026-09-30. Generic 2.54 mm
headers, shunts and M3 hardware aren't priced — the 120-piece assortment
linked in
[`../archive/hardware/BOM-bluetooth.md`](../archive/hardware/BOM-bluetooth.md)
covers every header on this board.

### Base board — every build · **~$19.12**

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | MCU carrier: **U1** ESP32-C6-DevKitC-1 (or equivalent) | [DigiKey](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-DEVKITC-1-N8/17728861) $9.00 | [AliExpress](https://www.aliexpress.us/item/3256806563223929.html) $7.72 · [AliExpress](https://www.aliexpress.us/item/3256809110312291.html) $3.99 | **N16 (16 MB) recommended; N8 (8 MB) acceptable** — the firmware fits either, N8 just leaves less room for recordings and voicemail (see *Flash size*). DigiKey stocks the N8 only. AliExpress $7.72 = 32-pin **N16**, the known-good shape. $3.99 = 30-pin clone: **needs trace cuts + 2 solder bridges, and is untested here** |
| | 1 | **U4** AG1171 SLIC (line 0, @ **J9**) | [DigiKey](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) $8.50 | [AliExpress](https://www.aliexpress.us/item/3256804758849823.html) $3.79 | 14-SIP. DigiKey lead time 16 weeks — buy a spare |
| | 1 | **C16** 470 µF 16 V radial | [DigiKey](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) $0.42 | | **16YXJ470M8X11.5**, **U4** +VPWR bulk |
| | 1 | **J9** RJ12 horizontal, 6p2c | [DigiKey](https://www.digikey.com/en/products/detail/amphenol-cs-fci/87180-062LF/1001369) $1.07 | [AliExpress](https://www.aliexpress.us/item/3256810043215346.html) $3.38 / 10 pcs | **87180-062LF**. AliExpress: pick the **322** variant. Alt **54601-006** — [DigiKey](https://www.digikey.com/en/products/detail/amphenol-icc/54601-006/1158894) |
| | 1 | **J5** 2×4 (8-pin) 2.54 mm header | [DigiKey](https://www.digikey.com/en/products/detail/same-sky-formerly-cui-devices/SSK03-MPH-254-B4/28772142) $0.13 | | **SSK03-MPH-254-B4**. Fitted on every build at this price — a second line needs it and so does Bluetooth. Two 1×4 strips cut from existing stock do the same job |

### Add: second phone line · **+~$9.99**

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **U3** AG1171 SLIC (line 1, @ **J8**) | same as **U4** | same as **U4** | |
| | 1 | **C17** 470 µF 16 V radial | same as **C16** | | **U3** +VPWR bulk |
| | 1 | **J8** RJ12 horizontal, 6p2c | same as **J9** | same as **J9** | |
| | 4 | **J5** jumper shunts, 2.54 mm | | | Bridge all four pairs (1-2, 3-4, 5-6, 7-8) |

**A second line rules out microSD.** Line 1's SLIC and the microSD socket
share GPIO18–21, and **J5** is the switch between them: bridge all four
pairs for line 1, or leave them all open for microSD — never a mix. The
firmware enforces it. Put a card in with the board configured for two lines
and it **powers line 1 down** and raises a banner in the web UI until you
remove the card or switch the board to single-line + microSD in Settings →
Hardware.

Losing microSD costs recording space, not function — recordings fall back to
internal flash, ~4 MB by default.

### Add: Bluetooth companion · **+~$14.97**

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **J2** 8-pin male header, 2.54 mm | [DigiKey](https://www.digikey.com/en/products/detail/same-sky-formerly-cui-devices/SSK01-MPH-254-A8/28772164) $0.17 | | **SSK01-MPH-254-A8**, the companion link |
| | 1 | Carrier PCB | — | | **Not yet fabricated.** Its own KiCad project, `Bluetooth/Bluetooth_board/` |
| | 1 | **BM64** module + carrier parts | ~$14.80 | | See [`BOM-bluetooth.md`](../archive/hardware/BOM-bluetooth.md) |

Three things to know before committing to this option:

- **It needs a second PCB fabricated.** The carrier is its own board and
  hasn't been ordered yet.
- **The BM64 is surface-mount** — a 43-pad module with no leads to reach
  with an iron. Building the carrier yourself means a **reflow oven or
  hotplate**, or paying for assembly.
- **It uses J5.** The carrier's programming header mates with the
  line-1/microSD jumper header for one-time EEPROM setup — which is why the
  base board fits **J5** whether or not it has a second line.

### Optional: DC jack and power supply

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **J1** DC barrel jack, 5.5 × 2.1 mm | [DigiKey](https://www.digikey.com/en/products/detail/mpd-memory-protection-devices/EJ508A/2439547) $1.25 | [AliExpress](https://www.aliexpress.us/item/3256802445218613.html) $2.70 / 10 | **EJ508A**. The AliExpress jack is **verified to fit**; the EJ508A's pin pattern hasn't been checked against the `Barrel_Jack_PTH_Slot` footprint |
| | 1 | **D12** line TVS | [DigiKey](https://www.digikey.com/en/products/detail/littelfuse-inc/P6KE82A/688097) $0.75 | [AliExpress](https://www.aliexpress.us/item/3256809218310441.html) $2.70 / 20 | **P6KE82A**, 70 V standoff / 113 V clamp / 600 W. **One per board, not per line.** Recommended — see below. **Package is DO-204AC (DO-15), not DO-41**: check against the board's `D_DO-41_..._P10.16mm` pads (the span is generous; the 0.8 mm lead may want a bigger drill) |

**Either input works, and both want ~2 A.** The two 5 V sources are
diode-ORed onto one rail, so use whichever is convenient:

- **USB-C** into the DevKit's own port — from a **2 A** charger or port.
- **DC jack** — a **2 A**, 5.5 × 2.1 mm centre-positive supply. It arrives
  through the OR diode, so a **5.5–6 V** brick covers the ~0.4 V drop better
  than a bare 5 V one.

2 A sizes the worst case, not the average: each AG1171 draws roughly 240 mA
running and considerably more while ringing, with the ESP32's WiFi bursts on
top. One idle line is happy on far less — but a two-line board ringing both
phones mid-transmit is what browns out an undersized supply, and a brownout
during a ring looks exactly like a mysterious reboot.

**Fit the jack if** the board lives somewhere permanent, or you'd rather not
tie up a USB port. **Skip it** if it will always hang off a laptop or
charger.

### Is the line TVS optional?

**Optional, but recommended.** Nothing in normal operation depends on it: it
clamps at 113 V, well above anything this board produces (48 V loop feed,
~60 Vrms ring, ~90 V peak). It earns its place the day something goes wrong,
and there are two realistic ways for that:

- **Someone plugs a board jack into a real phone line.** The RJ12 jacks look
  exactly like wall jacks, and this board is its own exchange — it expects to
  *drive* tip/ring, not to meet a live telco line with its own ring voltage
  on it. That's an easy mistake for a guest, a housemate, or future-you in a
  hurry.
- **Surges from outside wiring.** Lightning-induced transients and ESD get
  onto house and outside-plant cabling and arrive down the phone cord.

If every phone sits in the same room on a short cord and the jacks can never
reach house or telco wiring, leave it off. Otherwise populate it: **$0.75
protecting an $8.50 SLIC** with a 16-week re-order time is cheap insurance,
and the failure it prevents is a dead board, not a glitch.

### Optional: the spare headers

Neither is needed for any feature — both are footprints brought out for
people who want to hack on the board. Generic 2.54 mm pin headers.

| Ref | What it is | What it's for |
|-----|-----------|---------------|
| **J7** | 1×3 audio monitor out | The codec's second output pair, mirroring the earpiece volume. Scope it, feed a powered speaker, or listen in on a call without picking up a handset. **Dead until C10/C11 are populated** — see below |
| **J3**, **J4** | 1×2 each, on GPIO16 and GPIO17 | The ESP32's spare UART pins — usable as general-purpose I/O (a button or switch input, an LED or relay output) or as a serial console |

**J3/J4 come with two catches.** The Bluetooth companion claims GPIO16/17
for its own UART, so those two options are mutually exclusive. And on the
**30-pin clone module** these pins aren't reachable at all once its traces
are cut — see below.

### MCU module: 32-pin or 30-pin clone

The footprint takes either, but they are not equivalent.

| | **32-pin (recommended)** | **30-pin clone** |
|--|--|--|
| Part | Genuine ESP32-C6-DevKitC-1, N16 or N8 | AliExpress clone, ~$3.99 |
| Fitting | Drops straight in | **Cut two traces on the back, add two solder bridges** |
| Native USB | Works — flashing and console | Its USB pins land on other nets; console goes to UART instead |
| **J3**/**J4** spare UART | Available | **Not available** after the trace cuts |
| Status | Known good | **Untested here** |

The clone saves about $4 and is the cheapest way to 16 MB of flash, but it
costs native USB, the spare UART header, and a known-good starting point.
Use the genuine module unless you have a reason not to, and if you do use a
clone, expect to debug it. The pad swap and the jumpers it needs are written
up in
[`../archive/hardware/ANALOG_REDIAL_SMD5.md`](../archive/hardware/ANALOG_REDIAL_SMD5.md).

### Flash size: 16 MB vs 8 MB

**The firmware fits either way** — the app is ~1.5 MB against a 1.75 MB OTA
slot. What shrinks on an 8 MB part is room for audio. The current 16 MB
layout (`firmware/partitions.csv`) spends 1.75 MB × 2 on OTA slots, 6 MB on
recordings (`/assets`) and 2 MB on built-in clips (`/bundled`), and still
leaves ~4.5 MB unused.

**What to change for an 8 MB part** — three files, no code:

| File | From | To |
|------|------|-----|
| `firmware/partitions.csv` — `storage` | `0x600000` (6 MB) | **`0x2A0000`** (2.625 MB) |
| `firmware/partitions.csv` — `bundled` | `0x200000` (2 MB) | **`0x1C0000`** (1.75 MB) |
| `firmware/sdkconfig.defaults` (and `sdkconfig.smd5*.defaults`) | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` / `="16MB"` | **`..._8MB=y`** / **`="8MB"`** |
| `CONFIG_STORAGE_RECORDING_CAP_BYTES` (`components/storage/Kconfig`, default) | `4194304` (4 MB) | **`2097152`** (2 MB) |

That budget: 0x20000 for bootloader/tables/NVS, 1.75 MB × 2 OTA slots
(0x3A0000), leaving 4.375 MB to split between `bundled` and `storage`.
`bundled` holds ~1.55 MB today, so 1.75 MB keeps a little headroom; the rest
goes to recordings.

**Want more recording room?** Drop to a single app partition (no `ota_1`)
and `storage` grows to ~4.25 MB — at the cost of web-UI firmware updates,
which write to the inactive slot. USB flashing is unaffected either way.

After editing, rebuild from scratch (`idf.py fullclean build`): a
`sdkconfig.*.defaults` change does nothing to an already-generated
`sdkconfig`, so a non-default board's build directory has to be deleted and
regenerated — see `CLAUDE.md`'s build-directory rules.

---

## SMT parts (JLCPCB-assembled)

The full SMT list isn't reproduced here — it's commodity 0402 passives and
diodes, and the authoritative version is the KiCad project itself
(`hardware/kicad/Rotary_phone_SMD4/`; export BOM + CPL with the JLCPCB Tools
plugin). Only the parts worth knowing about by name are listed below.

| Qty | Component | JLC (LCSC) | Notes |
|-----|-----------|------------|-------|
| 1 | **U2** ES8388 audio codec | [C365736 (~$1)](https://www.lcsc.com/product-detail/C365736.html) | **Extended.** The board's whole audio path — both phone lines' earpiece/mic and the monitor header run through it |
| 1 | **Card1** microSD socket, TF-01A | [C91145 (~$0.20)](https://www.lcsc.com/product-detail/C91145.html) | **Extended.** Build **B** only |

Everything else — the tip/ring bridge diodes, I²C pull-ups, SHK dividers,
codec decoupling, SLIC coupling caps — is Basic-library parts costing
fractions of a cent, and the KiCad BOM export lists them all.
**Budget ~$2 in SMT parts plus ~$3 per unique Extended part** (ES8388
always; **Card1** on build B).

### C10/C11 — the monitor header's coupling caps

These AC-couple the ES8388's second output pair (`LOUT2`/`ROUT2`, which the
firmware mirrors from the earpiece volume) to **J7**, each through a 47 kΩ
bleed to GND. They were 100 µF 1206 — an Extended part sized for driving
16–32 Ω headphones directly.

**As built they're a 5 mm-pitch disc footprint, left unpopulated**
(2026-09-30), which keeps J7 available later without paying the Extended fee.
A radial electrolytic footprint was tried first and the cans were simply too
big for the space. The schematic carries **47 µF** as the nominal value, but
**fit 1 µF** to listen to or scope the header — against the 47 kΩ bleed
that's a ~3 Hz corner, far below the 300–3400 Hz phone band, and a
non-polarized film or ceramic part has no orientation to get wrong. Only
driving headphones directly needs the full 47 µF.

**Polarity, if an electrolytic is ever squeezed in: + toward the ES8388**
(`LOUT2`/`ROUT2`, U2 pins 14/15), − toward J7. The codec's outputs idle at
VMID ≈ AVDD/2 ≈ 1.65 V while the J7 side is held at 0 V by the bleed
resistor, so the codec side is always the positive one.

**Unpopulated, J7 is dead** — the bleeds tie it to ground and no signal
reaches it. Intended, not a fault.

### JLC order checklist

| | Step |
|---|------|
| | Generate Gerber + BOM + CPL (JLCPCB Tools) |
| | DNP every through-hole ref (the MCU carrier, SLICs, RJ12s, DC jack, TVS, bulk caps, all pin headers and mounting holes) plus **C10**/**C11** |
| | Extended: **C365736** (ES8388); build B adds **C91145** (Card1) |
| | Confirm build A vs. build B stuffing before generating CPL — line 1's SLIC support parts and **Card1** are mutually exclusive |

---

## Open items before ordering

- **ESP32-C6-DevKitC-1 (or equivalent) part number and price** — not yet
  pinned down; confirm the specific module/vendor and its onboard
  regulator's minimum 5 V input headroom (affects the DC jack voltage spec)
  before finalizing this line.
- **DC jack (J1), DigiKey option** — the AliExpress jack is verified to fit;
  the **EJ508A** pin pattern hasn't been checked against the
  `Barrel_Jack_PTH_Slot` footprint. Check before ordering that one.
- **Line TVS package** — **P6KE82A** is DO-204AC (DO-15) while the board's
  pads are DO-41. The 10.16 mm lead span has room to spare for the longer
  body; the open question is only whether the 0.8 mm lead clears the drill.
  Confirm before ordering.
