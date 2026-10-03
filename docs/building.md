# Building the Board

How to go from parts to a working board. Written for the **SMD4** board,
with **SMD5** differences called out inline — SMD5 is the next spin and has
not been fabricated or tested yet.

Once the hardware is together, continue to
[`getting-started.md`](getting-started.md) to install the toolchain and flash
it.

---

## 1. How you get a board

| Route | What you do |
|-------|-------------|
| **Pre-assembled** (most people) | The surface-mount side is already done — codec, resistors, capacitors, bridge diodes. You hand-solder the through-hole parts below. |
| **Order it yourself** | Send the Gerbers and BOM/CPL to JLCPCB for SMT assembly, then hand-solder the same through-hole parts. See [`bom.md`](bom.md). |
| **Fully from scratch** | Possible, but the ES8388 is a 28-pin QFN with a thermal pad — you'll want a hotplate or reflow oven, not an iron. |

Everything on the hand-solder list is through-hole and friendly to a regular
iron. There is no fine-pitch work left for you.

## 2. Tools

- Soldering iron with a fine conical or chisel tip, and solder
- Flush cutters for trimming leads
- Multimeter (continuity and DC volts) — **not optional**, see §6
- A 2 A USB-C supply or a 5.5–6 V DC brick
- One or two analog telephones with RJ11/RJ12 cords

## 3. What you're fitting

Pulled from [`bom.md`](bom.md), in the order it's easiest to solder.

| Order | Ref | Part | Notes |
|-------|-----|------|-------|
| 1 | **J5** | 2×4 pin header | Lowest profile, do it first |
| 2 | **J9** | RJ12 jack, line 0 | Snaps in; check it sits flat |
| 3 | **J8** | RJ12 jack, line 1 | Second line only |
| 4 | **D12** | P6KE82A TVS | Optional, see [`bom.md`](bom.md). **Polarized** (unidirectional): the body's stripe (cathode) goes at the silkscreen band end — pin 1, the `/P6K` bus, not GND |
| 5 | **C16** | 470 µF, line 0 | **Polarized** — stripe to the negative pad |
| 6 | **C17** | 470 µF, line 1 | Second line only, same polarity care |
| 7 | **U4** | AG1171 SLIC, line 0 | 14-pin SIL module, sits upright |
| 8 | **U3** | AG1171 SLIC, line 1 | Second line only |
| 9 | **J1** | DC barrel jack | Optional; chunky, needs a hot iron and a moment to wet |
| 10 | **U1** | ESP32-C6 DevKit | Tallest part, so it goes last |

**SMD5 adds J2**, the 8-pin Bluetooth companion header — fit it before the
DevKit, and only if you're using the Bluetooth option.

### Keeping the DevKit square

Tack one pin at each far corner first, check the module sits flat and
square against the board, then solder the rest. A module soldered in
crooked is a lot of pins to rework.

## 4. Choosing your build

Two parts of the board are mutually exclusive, and **J5** is the switch
between them.

| | Dual line | Single line + microSD |
|---|---|---|
| Fit | **U3**, **J8**, **C17** | Nothing extra |
| **J5** | **Bridge all four pairs** with shunts | **Leave open** — no shunts |
| Firmware | lines = 2, microSD off | lines = 1, microSD on |

Line 1's SLIC and the microSD socket share GPIO18–21, so only one can be
wired through at a time. Get this wrong and the firmware will tell you: put
a card in a board configured for two lines and it powers line 1 down and
raises a banner in the web UI.

## 5. ⚠️ Before you power anything on

This board generates real telephone-line voltages: about **48 V DC** on an
off-hook line and roughly **60 V RMS** while ringing, with peaks near 90 V.
That is enough to give you a memorable jolt.

- **Don't touch tip/ring, the RJ12 pins, or the SLIC pins while powered.**
  Ringing is the dangerous moment.
- **Never connect these jacks to house or telco wiring.** This board is its
  own exchange; it expects to drive the line, not meet a live one.
- Power down before changing jumpers or reseating the DevKit.

## 6. Check before power

Five minutes with a multimeter beats a dead SLIC.

1. **Look at it first.** Bridges between adjacent pins, especially on the
   RJ12 jacks and the DevKit headers.
2. **Electrolytic polarity** — C16/C17 stripe to the negative pad. Backwards
   is the classic way to vent one.
3. **Continuity check: +5 V to GND, and 3.3 V to GND.** Both should read
   open, not a short.
4. **Jumper check** — J5 bridged for two lines, open for microSD. Not a mix.
5. **SLIC orientation** — the AG1171s are keyed by their pin row; confirm
   pin 1 matches the silkscreen before you solder, because desoldering a
   14-pin SIL module is miserable.

## 7. First power-on

Power it with **nothing plugged into the phone jacks yet**.

- The DevKit's LED should come up. Nothing should get hot — if anything
  does, pull the power immediately and go back to §6.
- Measure the +5 V rail and the 3.3 V rail. Both should be within a few
  percent.
- Only once those look right, plug in a phone.

The board ships with no firmware, so it won't do anything useful yet.
That's expected — head to [`getting-started.md`](getting-started.md).

## 8. One known rework on SMD4

**Remove R14 and R12**, the two 15 kΩ resistors in the hook-switch dividers.
As built, they drop the off-hook signal to ~1.9 V, below the ESP32-C6's
2.475 V input-high threshold. Some boards read it as high anyway and some
don't — the symptom is a line whose audio works but whose handset lift is
never detected.

Leave **R15** and **R13**, the 10 kΩ series resistors, in place. Removing
both 15 kΩ resistors is confirmed on real hardware. Full explanation in
[`hardware-smd4.md`](hardware-smd4.md) §7.

**SMD5 fixes this in the design** (10 kΩ / 100 kΩ), so no rework there.

---

## See also

| Doc | What's in it |
|-----|--------------|
| [`bom.md`](bom.md) | What to buy, with prices and links |
| [`hardware-smd4.md`](hardware-smd4.md) | Schematic, layout, power tree |
| [`hardware-smd5.md`](hardware-smd5.md) | The next spin: Bluetooth, LDO, dual MCU footprint |
| [`getting-started.md`](getting-started.md) | Toolchain, flashing, first boot |
