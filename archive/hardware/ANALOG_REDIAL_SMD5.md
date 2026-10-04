# Analog Redial 5 board (SMD5) — dev notes

> **Status (2026-10-04): sent to fab, not yet tested.** Three boards went out
> together: this main board, the BM64 Bluetooth carrier
> (`Bluetooth/Bluetooth_board/`), and a BK3266 test board
> (`Bluetooth/BK3266_board_v1/`) for a first look at that module as a
> possible BM64 alternative. The fab version includes the re-routes these
> notes used to list as planned (checked against the schematic netlist
> 2026-10-04): MFB on GPIO15 via an 8-pin **J2** BM64 header with a 100 kΩ
> pull-down (R17), the JP3/JP2 MCLK/card-detect solder jumpers, and the
> 100 kΩ SHK divider. Not in it: the multi-function button.

KiCad project: `hardware/kicad/Analog Redial 5/`. These are working notes
per [`CLAUDE.md`](../../CLAUDE.md)'s public-vs-internal-docs split, not a
public build guide. Nothing here is proven until boards come back and get
bench-tested.

## What's different from SMD4

The headline change is the MCU module footprint: SMD5 accepts **either** a
genuine Espressif ESP32-C6-DevKitC-1 module **or** a cheaper AliExpress
clone module with a different physical pinout, via a dual-compatible
footprint (`Scott.pretty/ESP32-C6-DevKitC-dual.kicad_mod` for the real
module, `ESP32-C6-DevKitC-cheap.kicad_mod` for the clone). Everything else
(SLIC/codec/tip-ring layout, clearance rules) carries over unchanged from
SMD4 — see [`ROTARY_PHONE_SMD4.md`](ROTARY_PHONE_SMD4.md) and
[`docs/hardware-smd4.md`](../../docs/hardware-smd4.md) for that baseline.

## The module swap: what actually changes

Both module types are electrically the same chip (ESP32-C6), so the
schematic itself (`Analog Redial 5.kicad_sch`) only needs **one** symbol
(`PCM_Espressif:ESP32-C6-DevKitC-1`, ref `U1`) using the real module's
standard pad numbering. The clone module's PCB routes some of its own chip
pins to *different physical pad positions* than the real module does — the
footprint (not the schematic) is what's dual-compatible, by physically
placing each shared pad number at whichever location lines up with *both*
modules' silkscreens, wherever they agree.

**The one confirmed discrepancy** (found by directly comparing the two
modules side by side): pad positions 12/13 and 19/20 disagree between
vendors.

| Schematic pad # | Real Espressif module | AliExpress clone module |
|--|--|--|
| 12 | `GPIO2` | `GPIO12` / `USB_D-` |
| 13 | `GPIO3` | `GPIO13` / `USB_D+` |
| 19 | `GPIO12` / `USB_D-` | `GPIO2` |
| 20 | `GPIO13` / `USB_D+` | `GPIO3` |

In other words: wherever the real module's native-USB pins physically are,
the clone's `GPIO2`/`GPIO3` show up instead, and vice versa. **This is a
silicon-level constraint, not a Kconfig one**: on the ESP32-C6, the
USB-Serial-JTAG *peripheral* is hard-wired to GPIO12 (D-)/GPIO13 (D+) —
unlike UART/SPI/I²C, which route through a flexible GPIO matrix to whatever
pin you configure, native USB cannot be relocated to different pins in
software. That's exactly why Espressif's own schematic labels these pins
"GPIO12"/"GPIO13" first (with the USB function as a secondary annotation,
same as this project's own symbol does above) — they're ordinary GPIOs any
time the native-USB peripheral isn't in use. **With the real module, it is
in use**: native USB is the board's flash/console link (corrected
2026-09-28 — an earlier version of this note called it "the normal case"
that they're free). So don't put MFB or anything else on GPIO12/13.

**Practical consequence**: whichever physical pad position ends up wired to
this board's actual USB-C/micro connector will only carry real native-USB
signaling with the real Espressif module populated. With the AliExpress
clone installed in the same footprint, that same connector position instead
carries the clone's `GPIO2`/`GPIO3` — ordinary digital pins, not USB — so
the board's own USB port goes data-dead (still fine as power-only) on
clone-populated units. No electrical risk either way (both are just 3.3V
digital pins), just a functional loss of native-USB flash/console on that
variant.

**Layout rule this implies**: apart from the two nets handled by the
jumpers below, don't assign anything to pad positions 12/13/19/20 — one
module or the other puts live USB D-/D+ signaling there.

### Solder-jumper select for MCLK and card-detect (in the fab version)

Decided 2026-09-29; **in the board sent to fab** as **JP3** (`I2S_MCLK`:
pad 1 → footprint pad 12, pad 3 → pad 19) and **JP2** (`SD_CD`: pad 1 → pad
13, pad 3 → pad 20). With the jumpers moved to the clone side, a clone board
can run the normal `CONFIG_BOARD_TYPE_SMD5` build with native USB intact;
`SMD5_CHEAP` is only needed if they're left at the default. Only two nets need a pad in the
swapped group: `I2S_MCLK` and `SD_CD`. Give each a 3-pad solder jumper
(symbol `Jumper:SolderJumper_3_Bridged12`, footprint
`Jumper:SolderJumper-3_P1.3mm_Bridged12_Pad1.0x1.5mm`) so the same net can
reach *either* module's GPIO2/GPIO3:

| Jumper | Centre pad (common) | Default, pre-bridged | Alternate, open |
|--|--|--|--|
| `JP_MCLK` | `I2S_MCLK` → codec `MCLK` | footprint pad **12** = real module `GPIO2` | footprint pad **19** = clone `GPIO2` |
| `JP_CD` | `SD_CD` → `Card1` pin 9 | footprint pad **13** = real module `GPIO3` | footprint pad **20** = clone `GPIO3` |

Both positions land on **GPIO2/GPIO3 in firmware** whichever module is
fitted, so the pin map stops depending on the variant and native USB keeps
its own pads in both cases. The `Bridged12` footprints ship with pads 1-2
joined by a hair-thin trace, so the board has a working default straight
from the fab: cut that trace and blob 2-3 to switch. (Two 2-pad jumpers at
90° work too — the 3-pad part just reads as one A/B selector.)

- **Bridge one side only.** Bridging both shorts pad 12 to pad 19 — with
  the real module that's GPIO2 fighting USB D-.
- **Silkscreen each jumper** ("MCLK: ESP / CLONE"): a wrong one gives a
  board with no audio and no obvious cause.
- **Drop J3/J4 from pads 19/20** while in there. They tap those pads today,
  which with the real module puts header stubs on live USB D-/D+, and after
  this change they'd also hang off the MCLK/CD alternates.
- Signal integrity isn't a concern here (MCLK is 12.288 MHz, card-detect is
  DC). Don't extend the same trick to the USB pair itself.

**Symbol trap (hit 2026-09-29).** SparkFun's PCM symbol
`PCM_SparkFun-Jumper:SolderJumper_3_Bridged12_No_Silk` numbers its pins
**A=1, B=2, C=2** — pins B and C both claim pad 2, and pad 3 gets nothing.
Wire B and C to different nets and KiCad rejects it ("pad 2 connected to
multiple nets"); the SparkFun *footprint* is fine (pads 1/2/3). Use KiCad's
stock `Jumper:SolderJumper_3_Bridged12` instead, whose pins really are
1/2/3 — its letters run A=1, **C=2**, B=3, so wire by pin **number**, not
letter. Pad numbers match SparkFun's footprint either way, so swapping only
the symbol (Edit → Change Symbols, keep the Footprint field) needs no PCB
rework.

**Footprint variant:** `Jumper_3_NC-1_Trace_NoSilk` has its copper trace
between pads 1 and 2 — one gap closed, which is what's wanted.
`Jumper_3_NC-2_Trace_NoSilk` closes *both* gaps (all three pads shorted);
don't use it here.

**Firmware once the jumpers exist:** `CONFIG_BOARD_TYPE_SMD5_CHEAP` stops
needing its own pin map (`BOARD_I2S_MCLK`/`BOARD_SD_CD` go back to
GPIO2/GPIO3 for both variants) and `board_early_pin_init()`'s USB-PHY
detach can be deleted with it. Everything in "Clone variant" below then
collapses to just the console/BM64 question.

#### Getting the jumper wrong: what it looks like

**Nothing errors.** MCLK is an ESP32 *output*, so a jumper on the wrong
side just leaves it driving an open pad while the codec sees no clock at
all. I²C runs on different pins, so the codec still answers and the boot
log still prints `codec detect: ES8388 @ 0x10` and `es8388: init ok`. WiFi,
the web UI, hook detection and ringing all come up normally. The board
looks healthy and is stone deaf.

| Module | Jumper | Firmware | Result |
|--|--|--|--|
| Real | default (12/13) | `SMD5` | correct |
| Clone | alternate (19/20) | `SMD5` | correct — this is the point of the jumpers |
| **Clone** | **default (12/13)** | `SMD5` | **no audio**, microSD always "no card" |
| Clone | default (12/13) | `SMD5_CHEAP` | works, but it's the pre-jumper arrangement: no USB console, and a seated card kills USB |
| **Clone** | **alternate (19/20)** | `SMD5_CHEAP` | **no audio**, and no USB console either |
| **Real** | **alternate (19/20)** | `SMD5` | **no audio**, *plus* MCLK and the card-detect switch now sit on USB D-/D+ — with a card seated USB won't enumerate, so no flashing and no console until the card comes out or the jumper goes back |

In every "no audio" row, what you'd actually observe, in the order you'd
hit it:

- **No dial tone on any line**, on-hook or off. `tone 1000 3000 0` from the
  console plays nothing and `capture` reads flat zero on both lines.
- Everything non-audio still works — hook events, ringing, WiFi, web UI,
  the dial plan, recordings that never contain sound — which makes it look
  like a dead codec or a SLIC fault, and sends you down the wrong path.
- **microSD reports no card, always**, because card-detect is on the open
  pad too (internal pull-up, nothing pulling it down).

**Fastest check: scope the codec's MCLK pin (U2 pin 1).** 12.288 MHz should
be present any time the board is powered, regardless of call state. A dead
pin there with `es8388: init ok` in the log is this fault, not a dead
codec — the two use different pins, which is exactly why the boot log stays
clean. If MCLK is alive at the ESP32 pad but dead at the codec, the jumper
is on the wrong side.

### Real module: the default build

Nothing special. Native USB on pads 19/20 is the flash/console link exactly
as on SMD2/SMD4, and pads 12/13 are `GPIO2`/`GPIO3` = MCLK and card-detect.
Build `CONFIG_BOARD_TYPE_SMD5`. Don't put MFB — or anything else — on
GPIO12/13.

### Clone variant: what's different

All of this applies **only** to a clone-populated board built from the
current, jumper-less layout, where MCLK and card-detect land on the clone's
GPIO12/13:

- **Its own pin map** (`CONFIG_BOARD_TYPE_SMD5_CHEAP`, added 2026-09-28):
  `board_pins_smd4.h` maps MCLK → `GPIO12` and CD → `GPIO13`, and
  `board_early_pin_init()` (board.c) detaches the USB PHY from those pads,
  D+ pull-up included, before audio starts — ESP-IDF's GPIO driver won't do
  that on its own, and with the PHY still attached MCLK never reaches the
  codec. For the first ~1 s after reset the ROM still owns the pads: MCLK is
  silent and CD reads "no card". Both harmless.
- **No USB console.** The port disappears from the host ~1 s into every
  boot. Use the web UI's Diagnostics tab, or UART0 — but see the BM64
  conflict below.
- **Flashing over native USB still works**, because the ROM owns GPIO12/13
  from every reset. To get back into the ROM without the BOOT button, the
  `download` console command (bringup.c, 2026-09-29) quiets the codec and
  SLICs, sets `LP_AON_SYS_CFG_REG`'s force-download-boot bit, and restarts.
  That bit survives a reset, so **power-cycle the board when flashing is
  done**. BOOT + RESET is the same thing in hardware, and the escape hatch
  when firmware is too broken to reach the command.
- **A seated microSD card blocks USB entirely.** `SD_CD` is a switch that
  shorts its pin to GND with a card in, and that pin is USB **D+** — held
  low, USB can't enumerate at all, with no obvious symptom. A series
  resistor can't fix it: anything small enough to keep D+ usable is too
  small to read as a valid low. Pull the card before flashing, or don't
  route `SD_CD` at all — `sdcard.c` compiles to a card-always-present check
  when `BOARD_SD_CD` is undefined, and microSD is mutually exclusive with
  line 1 via J5 anyway.
- **UART0 console vs. the BM64.** `sdkconfig.smd5_cheap.defaults` puts the
  console on UART0 (GPIO16/17) — the same pins as the BM64 UART on J2.
  They can't share. With `download` making native USB a workable flashing
  path, "BM64 keeps GPIO16/17, no UART console" is the cheaper answer.
- **Don't plug a cable into the clone module's own USB-C** while the app
  runs: 12.288 MHz MCLK is sitting on its D- pin.

**Other duplicate pad-number entries in the `.kicad_mod` files** (15, 18,
22-31) are *not* additional known discrepancies — they're just how the dual
footprint records that both vendors happen to agree at that position. Only
the 12/13 ↔ 19/20 swap above is a confirmed, deliberately-checked mismatch.
Before trusting this for a production run, re-verify pad-by-pad against a
real sample of the clone module (continuity/multimeter check, or flash a
firmware build that toggles a known GPIO and probe the physical pad) — a
dual-footprint pad remap like this is inherently easy to get subtly wrong,
and this project has exactly one clone-module sample's silkscreen to go on
so far.

## Open concern: clone module's onboard 3.3V regulator may be undersized

Flagged 2026-09-22, not yet measured: the AliExpress clone module carries a
smaller onboard 3.3V regulator than the real Espressif module's. This
board's 3.3V rail has to feed more than just the MCU module itself once the
BM64 companion and ES8388 codec are both wired in through J2/the
audio path — worth checking, before relying on the clone variant for
anything beyond bench testing, whether that regulator can actually supply
everything at once under worst-case load (WiFi TX bursts + codec + BM64
all drawing simultaneously). If it can't:
- The simplest fix is adding a separate/second 3.3V regulator on the main
  board for the codec (and BM64, if its own supply also comes from this
  rail) rather than relying on the clone module's onboard one for
  everything.
- If that's not workable, the practical fallback is: don't populate BM64 on
  a clone-module board at all (real-module boards only for the Bluetooth
  companion feature) until this is measured and resolved.
Needs an actual current-draw measurement on a real clone-module sample
before this can be resolved either way — don't assume either outcome
without measuring.

## Flashing/console tradeoff for clone-populated boards

Partly superseded by the `download` command and the jumper plan above —
native USB *is* usable for flashing a clone-populated board, it's the
interactive console that isn't. The UART0 path below remains the fallback,
and stays the only option if firmware ever comes up too broken to reach
`download` and the BOOT button isn't accessible. See
[`CLAUDE.md`](../../CLAUDE.md)'s "Building/flashing for a specific board
type" section for the general multi-board-type gotchas, none of which are
specific to SMD5. The concrete plan for SMD5:

- **Flash over UART0 instead**, which works identically regardless of which
  module is populated: `GPIO16` (U0TXD) / `GPIO17` (U0RXD) — same pins this
  project already reserves as `BOARD_UART_TX`/`BOARD_UART_RX` for the BM64
  companion link (see below) — plus `EN` (reset) and `GPIO9` (boot-mode
  strap, pulled low at reset to enter download mode). An external
  FTDI-style USB-serial adapter on those four signals (+3V3/GND) drives
  `esptool`/`idf.py flash` exactly like the native-USB path does on
  SMD2/SMD4.
- **Plan**: flash + bench-test each board over that UART0 connection before
  or right after assembly (this is where bugs get caught, same commands as
  any other board type), then rely on OTA (`/ota_upload`) for every update
  after that, once firmware is confirmed good. Don't ship untested firmware
  to a clone-populated board expecting to debug it later over its own USB
  port — that port won't be reachable once assembled.
- **Two Kconfig board types exist for this** (added 2026-09-22, built and
  verified against both — see `firmware/README.md`'s multi-board-type
  section): `CONFIG_BOARD_TYPE_SMD5` (real module, native USB console, same
  as SMD2/SMD4) and `CONFIG_BOARD_TYPE_SMD5_CHEAP` (clone module,
  `sdkconfig.smd5_cheap.defaults` leaves the console on plain UART0 instead
  of overriding to native USB — the one SMD board type where that's
  correct, not a bug). The two no longer share a pin map: as of 2026-09-28
  `board_pins_smd4.h` gives the clone variant MCLK/CD on GPIO12/13 (see
  "Clone variant" above), and as of 2026-09-29 both SMD5 variants put the
  BM64's MFB on GPIO15. Building each needed the `wm8731`/`es8388`
  components' own per-board-type CMakeLists.txt gates extended too (they
  don't follow `board_pins.h`'s routing automatically) — worth remembering
  if a 3rd variant ever gets added.

## BM64 Bluetooth companion header (J2)

As sent to fab: an 8-pin header, `J2` (`PINHD_1x8_Male`), mating with `J1` on
the BM64 carrier (`Bluetooth/Bluetooth_board/`). Pinout checked against both
schematic netlists 2026-10-04 -- the two boards agree:

| J2 pin | Main board | Carrier (J1) |
|--|--|--|
| 1 | `+3.3V_BM` from **U5** (XC6206 LDO, its own rail) | BM64 `BAT_IN` |
| 2 | GPIO17 / U0RXD (ESP receives) | `espUartRx` → BM64 `HCI_TXD` |
| 3 | GPIO16 / U0TXD (ESP transmits) | `espUartTx` → BM64 `HCI_RXD` |
| 4 | `MFB` → GPIO15, **R17** 100 kΩ to GND | BM64 `MFB` |
| 5 | via **C22** ← `Line0Vout` (SLIC U4 pin 10) | BM64 `MIC_P1` |
| 6 | via **C24** ← GND | BM64 `MIC_N1` |
| 7 | via **C23** → **R16** 1 kΩ → `Line0Vin` | BM64 `AOHPL` |
| 8 | GND | GND |

Audio never passes through firmware -- both directions are analog between
the BM64 and line 0's SLIC; the ESP32 only drives the UART and MFB.
`bt_companion_line()` (`telephony.c`) always returns line 0, matching this.

The carrier also has a 2-pin `J2` of its own (`P2_0`, `RST_N`) for
one-time EEPROM programming through the main board's J5 -- see item 2
below.

(History: the first draft was a 6-pin `J12` with no MFB, which was
bench-wired separately with a fixed pull-up during 2026-09-21's bring-up.)

**Companion daughter-board**: `Bluetooth/Bluetooth_board/` is the BM64
module's own carrier board; its `J1` mates with J2 (same 8-pin order).

### Re-route (decided 2026-09-27, in the fab version)

1. **MFB gets its own J2 pin (4), on GPIO15** (decided 2026-09-29, done in the
   schematic). GPIO15 is the only genuinely spare pin in this map: GPIO8
   drives the DevKit's RGB LED and GPIO9 is the download-mode strap, where
   MFB's pull-down would boot the board into download mode every time.
   GPIO15 is a strapping pin too, but it only selects the JTAG signal source
   and only when an eFuse that ships unburned is set, so a 100 kΩ pull-down
   on it is harmless. **Firmware: done** -- `board_pins_smd4.h` defines
   `BOARD_BT_MFB_PIN = GPIO_NUM_15` plus `BOARD_BT_MFB_DEDICATED` for both
   SMD5 variants, and because MFB no longer borrows a shared pad it works on
   a 2-line/microSD board; only RST_N/P2_0 (still GPIO19/20, via J5) keep the
   single-line gate.
2. **EEPROM programming goes through J5, not J2.** The BM64's IBDK mode
   needs RST_N and P2_0 (plus MFB) driven at reset -- rare, one-time-per-
   module setup. Rather than route those permanently, the Bluetooth board
   gets a *separate* small header that mates to J5 (the line-1/microSD
   jumper header) just for programming. Procedure: pull J5's four shunts
   (line 2's SLIC, if stuffed, is then electrically disconnected), plug the
   Bluetooth board's programming header into J5, set Settings -> Hardware to
   one line + BM64 fitted, run the EEPROM check, then unplug and re-fit the
   shunts. Firmware already supports this path: `bt_companion_ibdk_enter()`
   drives GPIO18-20 and refuses on a board configured for two lines.
3. **Hole size for the solderless programming header.** The J5-mating
   header is used once per BM64 and then removed, so a friction fit is
   tempting. A standard 2.54 mm header pin is a **0.64 mm square post** —
   0.64 mm across the flats, **~0.90 mm across the corners** — and plating
   takes roughly 0.05 mm off the drilled diameter:

   | Intent | Drill | Finished | Feel |
   |--------|-------|----------|------|
   | Normal, soldered | 1.0 mm | ~0.95 mm | Free-moving |
   | **Snug** | **0.9 mm** | ~0.85 mm | Corners bite lightly; still solderable |
   | Press fit | 0.85 mm | ~0.80 mm | Firm, needs deliberate pushing |

   **The catch is tolerance, not geometry.** JLCPCB's plated-hole tolerance
   is around ±0.08 mm, which is larger than the interference being aimed
   for: a nominal 0.85 mm finished hole can land anywhere from ~0.77 mm
   (a genuine press fit) to ~0.93 mm (the pin drops through). Friction alone
   is therefore not repeatable across a batch.

   **The staggered-hole alternative.** Instead of making the hole smaller
   than the pin, keep a normal 1.0 mm hole and move alternate holes a little
   to either side of the row's centreline. Top view of the PCB:

   ```
   Straight row — every hole centred on the line:

        ┌───────────────────────────────────────┐
        │    ◯     ◯     ◯     ◯     ◯     ◯    │
        └───────────────────────────────────────┘
             └2.54┘
        Grip depends on the hole being SMALLER than the
        pin's 0.90 mm corner-to-corner diagonal.

   Staggered row — alternate holes nudged off the line:

        ┌───────────────────────────────────────┐
        │    ◯           ◯           ◯          │  ← odd pins, "up"
        │ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ ‑ │  ← nominal centreline
        │          ◯           ◯           ◯    │  ← even pins, "down"
        └───────────────────────────────────────┘
        Grip depends on how far the holes are OFFSET.
   ```

   The header's pins are moulded into a rigid strip in a dead-straight line.
   The holes are not straight, so the pins cannot all sit centred: each one
   is pushed against one wall of its barrel and pushes back. Side view, pins
   entering the board:

   ```
        plastic strip
        ████████████████████████
          │    │    │    │    │        pins: straight and stiff
          ▼    ▼    ▼    ▼    ▼
        ──┬────┬────┬────┬────┬──
          │→   │   ←│→   │   ←│        each pin leans on one side
        ──┴────┴────┴────┴────┴──      of its (normal-sized) hole
   ```

   **Why that tolerates a sloppy fab better.** An interference fit is
   *dimension*-controlled: grip comes from ~0.05 mm of pin-vs-hole overlap,
   and the fab's ±0.08 mm diameter tolerance is larger than the thing you're
   relying on. A staggered fit is *force*-controlled: grip comes from
   elastically deflecting the pins, and the deflection is set mostly by the
   offset you drew. Hole **position** tolerance is also tighter than hole
   **diameter** tolerance, so the feature doing the work is the better-
   controlled one. Pick an offset bigger than the diameter tolerance and an
   oversized hole only softens the grip instead of eliminating it.

   **It's a maker technique, not an engineered press fit.** Real press-fit
   connectors use compliant pins — an "eye of the needle" section designed
   to deform elastically. A plain square post isn't, so it's the *pin* that
   bends, and too much offset will either refuse to insert or take a
   permanent set. For a once-per-module programming header that's an
   acceptable trade; for anything permanent, use pogo pins.

   **Prototype the offset rather than trusting a number.** Somewhere between
   **0.05 mm and 0.15 mm** each way is the range worth trying, but how much
   a given header tolerates depends on its pin stiffness and how much free
   length sits between the plastic and the board. Put a test strip on the
   panel — a few rows of 8 holes at 0.05 / 0.10 / 0.15 mm offset — and find
   out which grips without fighting you. That costs nothing on a board
   you're already ordering.

   **Or just tack-solder two end pins** and lift them afterwards. It's a
   once-per-module operation, and this remains the pragmatic answer.

   **Why this matters more than usual here:** a friction contact through
   plating isn't gas-tight and can go intermittent or high-resistance. This
   header's whole job is writing the BM64's EEPROM — a dropout mid-write
   leaves the module half-configured, which is a much worse failure than a
   dropped console character.

4. **Add the Vin protection network** -- series resistor + clamp diodes to
   +5 V at the Line0Vin tap -- designed in
   [`archive/BLUETOOTH_AUDIO_UPGRADE.md`](../BLUETOOTH_AUDIO_UPGRADE.md)
   after a SLIC's audio hybrid died during BM64 bench work on SMD4. The
   existing 10 µF series cap on J12 pin 2 alone is not protection.

Parts to order for this option (module, headers, the LDO and its passives,
and which BM64 suffix to buy): [`BOM-bluetooth.md`](BOM-bluetooth.md).

### BM64 connection table (2026-09-28 review of the 6-pin J12 draft -- historical; J2 table above is current)

Pulled from `kicad-cli sch export netlist` of `Analog Redial 5.kicad_sch`
and `Bluetooth/Bluetooth_board/Bluetooth_board.kicad_sch` -- "Now" is what
the schematics say today, "Change" is the recommendation.

| BM64 pin | Carrier (J2) | SMD5 side | Now | Change |
|--|--|--|--|--|
| 22 BAT_IN | J2.1 | J12.1 +3.3V | direct, no decoupling on carrier | add 10 µF + 100 nF at BAT_IN on the carrier. Spec is 3.2-4.2 V; the DevKit 3.3 V rail measured 3.15 V -- see below |
| 7 AOHPL | J2.2 | J12.2 → C23 → Line0Vin (U4.9) | C23 10 µF 0805 | **C23 → 1 µF 0402 (any ≥6.3 V Basic part); add R_prot 1 kΩ 0402 between C23 and Line0Vin.** BAV99 clamp optional (decided 2026-09-28: skip/DNP -- soldered-down grounds, no live re-plugging) |
| 9 MIC_P1 | J2.3 | J12.3 → C22 → Line0Vout (U4.10) | C22 1 µF 0402 | keep. Optional 1 kΩ in series (−0.4 dB into 24 kΩ) |
| 8 MIC_N1 | -- | -- | **floating** | **1 µF 0402 to GND on the carrier** (AC ground; don't short it to GND, it's internally biased) |
| 10 MIC_BIAS | -- | -- | NC | keep NC (no electret) |
| 5 AOHPR / 6 AOHPM | -- | -- | NC | keep NC (single-ended mono) |
| 20 HCI_TXD | J2.4 | J12.4 → GPIO17 (U0RXD) | direct | OK |
| 19 HCI_RXD | J2.5 | J12.5 ← GPIO16 (U0TXD) | direct | OK |
| 14, 43 GND | J2.6 | J12.6 | | OK |
| 26 MFB | J6.1 (J6.2 = GND) | **GPIO15**, 7th J12 pin (decided 2026-09-29) | jumper/button only | **Never tie it high permanently**: it's the BM64's power button, and held from power-up it triggers a MASTER RESET that wipes pairings (found 2026-09-28). Add a **100 kΩ pull-down** so it idles low while the ESP32 is in reset/booting -- the internal pull-down isn't a substitute, it only exists once firmware configures the pin (~1 s after every reset), and MFB floats until then. Firmware follows datasheet Fig 5-3/5-4: high 400 ms for power-on, then idle low, 1 ms wake pulse + >=150 ms high around each UART command |
| 13 RST_N | -- | -- | NC (internal POR) | **keep NC in normal use** (or, if ever GPIO-driven for the Fig 5-5 power-off sequence, give it an external pull-up so it can't float low during an ESP32 reset) -- confirmed 2026-09-28 on newphone: bench-wired to GPIO19, it reset the BM64 on every ESP32 reboot and the phone's pairing was lost each time; with the wire off, pairing survives reboots. Only connect it for IBDK programming via J5 |
| 32 EAN / 41 P2_0 | J10 / J11 (to GND) | -- | jumpers | OK for programming |

**Why 1 µF is enough for C23.** The AOHPL cap drives R_prot + (Vin's 60 kΩ
in parallel with the 10 nF C12 to the codec's LOUT1) -- at least ~5 kΩ
at 3.4 kHz and ~29 kΩ at 300 Hz. 1 µF against that puts the corner well
under 30 Hz even at the band top, and around 5 Hz at the bottom. After X5R DC-bias
derating (~2.4 V across it: AOHPL's ~1.5 V bias vs Vin's ~3.9 V) it's still
~0.4-0.6 µF even for a 6.3 V 0402 part. The 10 µF was chosen for a "few
hundred ohm" Vin that turned out to be 60 kΩ. Dielectric/voltage barely
matter here: 1 µF 0402 only exists as class II (X5R/Y5V) anyway, the cap
sees at most ~4 V DC, and derating only moves an already-tiny low-frequency
corner. Higher voltage rating is just margin against faults.

**Don't bump line 0's C12 (codec → Vin) to 100 nF** as suggested for the
265 Hz corner in `BLUETOOTH_AUDIO_UPGRADE.md`: during a BT call the codec's
LOUT1 is an active low-impedance output, so C12 shunts the BM64's signal at
the Vin node. With 10 nF that costs ~1.5 dB at 3 kHz through the 1 kΩ R_prot;
100 nF would cost ~9 dB. Line 1's C15 has no BM64 branch and can go to
100 nF. (Firmware alternative: power down LOUT1 during passthrough with
DACCONTROL23 VROI=40 kΩ -- untested.) The reverse direction -- an active
AOHPL shunting dial tone through the 1 kΩ -- evidently doesn't happen when
idle (dial tone is fine on newphone with the BM64 fitted), but a quick check
is worth doing: dial-tone Vpp at Vin with the BM64 powered and paired vs.
unplugged.

**Supply.** BAT_IN's minimum is 3.2 V (UVP 2.9 V) and the datasheet
recommends, for a non-battery supply, a voltage supervisor holding RST_N low
(open-drain, 2.93 V threshold) before BAT_IN drops to 2.7 V. The DevKit's 3.3 V rail
already measured 3.15 V on dualphone2, and WiFi TX bursts dip it further.
Candidate contributor to paired devices vanishing across ESP32 reboots.
Options: a dedicated LDO from +5V at 3.6-3.8 V for BAT_IN (HCI UART levels
come from the module's internal VDD_IO, so 3.3 V logic is unaffected), and/or
the supervisor on RST_N.

**Decided 2026-09-28: dedicated LDO for the BM64.** +5V → fixed 3.3 V LDO
(XC6206P332MR, SOT-23, 1 µF in/out -- or ME6211/AP2112K if Basic stock
changes) → its own net (e.g. `+3V3_BT`) → J12.1. **Not tied to the ESP32's
+3.3V rail**: two regulators on one net fight, and isolating the BM64's RF
bursts from the DevKit's (small, on the clone) regulator is the point.
Current budget from the datasheet: ~0.6 mA standby, 1.2 mA pairing,
~16 mA in a SCO call or A2DP (Class 1 radio, so short TX peaks well above
that -- size for ~100 mA). Microchip's own MCU-hosted EVB measurements were
taken at BAT_IN = 3.3 V, so 3.3 V is a supported operating point when it's
a clean, dedicated rail.

### Planned: a multi-function button (not in the fab version)

Decided 2026-10-01. One user-facing button, so the board is usable without
reaching for the DevKit's BOOT button or a serial console.

- **Wire it to a spare ESP32 GPIO**, not to the BM64's `MFB`. MFB is the
  module's power button: a human-length press on it is exactly the gesture
  that triggered the master reset which wiped pairings (see the MFB note in
  the BM64 table above). A GPIO lets firmware debounce it and decide what a
  press *means*, then call the existing pairing command.
- **Candidate pins: GPIO12 or GPIO13** — but only on a board where the
  MCLK/card-detect jumper work above has freed them, and only on the
  genuine-module variant, where they're native USB. Check against the pin
  map before committing; GPIO15 is now MFB.
- **Gestures**, mirroring what the firmware already does on BOOT:
  | Press | Action |
  |-------|--------|
  | Single click | Start Bluetooth pairing |
  | ~3 s hold | Rescue Wi-Fi AP (credentials kept) |
  | ~10 s hold | Factory-reset Wi-Fi |
- **Mechanically**, a side-mounted tactile switch reachable through the
  enclosure wall is the obvious form, since the board is 5 mm longer than
  SMD4 and needs a new case anyway.

Firmware already has the two hold gestures in `board_boot_button_start()`
(board.c) — a second button would reuse that watcher with its own GPIO and a
click handler added.

### SHK divider: change 15 kΩ → 100 kΩ (found 2026-09-27, in the fab version)

SMD5 inherits SMD4's 10 kΩ / 15 kΩ SHK divider, which was sized for a 5 V
SHK swing. The AG1171's SHK is actually a ~3.3 V output with a 3.3 kΩ
internal pull-up (VOH 2.8 V min), so the divider lands off-hook at ~1.9 V
-- below the ESP32-C6's 2.475 V input-high threshold. On dualphone2, line
1 (GPIO19) never registered the handset lift at all while line 0 (GPIO6)
worked with the identical voltage, purely by pad-threshold luck. **Keep the
10 kΩ, change the 15 kΩ to 100 kΩ on both lines** (~2.9 V off-hook, still
a defined low with the SLIC powered down). Both values are JLCPCB Basic
parts in common footprints, which was the constraint behind the original
15 kΩ -- confirm at the chosen footprint. Details in `docs/hardware-smd4.md`.

## Open questions

- Full pad-by-pad diff between the two module footprints hasn't been
  independently re-verified beyond the one 12/13↔19/20 swap found by direct
  inspection — worth doing before ordering a batch of boards.
- ~~MFB wiring (fixed pull-up vs. GPIO-controlled)~~ -- decided: its own
  GPIO on an expanded J12 (see the planned re-route above). Which GPIO is
  still open.
- Power sequencing between the BM64 and the SLICs -- see the open question
  in `archive/BLUETOOTH_AUDIO_UPGRADE.md`.
