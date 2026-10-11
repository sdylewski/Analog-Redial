[Home](../README.md) · [1 · Get a board](board.md) · **2 · Parts & assembly** · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md)

# 2 · Parts

The parts you solder yourself. The surface-mount parts come already placed
on the board from the fab ([step 1](board.md)). Once you have everything,
continue to [assembly](building.md).

Mark rows done in the leading empty column as you buy them.

Reference designators: **U1** = ESP32-C6-DevKitC-1, **U3**/**U4** = AG1171
SLICs, **J8**/**J9** = their RJ12 jacks, **J1** = DC jack, **J5** =
line-1/microSD jumper header, **Card1** = microSD socket.

---

## Build options

| Build | Lines | microSD | J5 jumpers | Parts |
|-------|-------|---------|------------|-------|
| **Dual line** | **J9** + **J8** | No | All four pairs bridged | Base board **+ second line** |
| **Single line + microSD** | **J9** only | **Card1** | Open, no shunts | Base board only |

**You can have a second line or microSD, not both.** Line 1's SLIC and the
microSD socket share the same four ESP32 pins, and **J5** is the switch
between them. The firmware checks: put a card in a board set up for two
lines and it shuts line 1 off and shows a warning in the web interface until
you remove the card or switch the board to single-line in Settings →
Hardware.

Without microSD, recordings go to internal flash, about 4 MB.

---

## Base board: every build · about $19

**Shortcut: [DigiKey parts list for a two-line board](https://www.digikey.com/en/mylists/list/WF3K3HFAQY)**,
ready to add to a cart. For a single-line board, leave out the parts in
*Second phone line*.

DigiKey prices are for quantity 1, checked September 2026, before shipping
(about $5). Generic 2.54 mm headers and M3 hardware aren't priced.

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **U1** ESP32-C6-DevKitC-1 | [8 MB, 32-pin](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-DEVKITC-1-N8/17728861) $9.00 | [16 MB, 32-pin](https://www.aliexpress.us/item/3256806563223929.html) $7.72 <br> 16 MB 30-pin $4 variant on Ali/Amazon, requires a board mod and 5V DC jack power.  | **16 MB recommended, 8 MB fine.** 8 MB just leaves less room for recordings (see [Flash size](#flash-size-16-mb-or-8-mb)). DigiKey stocks the 8 MB only. Use the **32-pin** module if possible; see [Microcontroller module](#microcontroller-module-32-pin-or-30-pin-clone) |
| | 1 | **U4** AG1171 SLIC (line 0, at **J9**) | [DigiKey](https://www.digikey.com/en/products/detail/silvertel/AG1171/21187236) $8.50 | [AliExpress](https://www.aliexpress.us/item/3256804758849823.html) $3.79 | 14-pin SIL module. DigiKey lead time can be 16 weeks, so buy a spare |
| | 1 | **C16** 470 µF 16 V radial electrolytic | [DigiKey](https://www.digikey.com/en/products/detail/rubycon/16YXJ470M8X11-5/3134104) $0.42 | [AliExpress](https://www.aliexpress.us/item/2255799897996539.html) $1.99 / 20 | Rubycon **16YXJ470M8X11.5**, or any 470 µF ≥16 V that is 8 mm across with 3.5 mm lead spacing |
| | 1 | **J9** RJ12 jack, horizontal, 6p2c | [DigiKey](https://www.digikey.com/en/products/detail/amphenol-cs-fci/87180-062LF/1001369) $1.07 | [AliExpress](https://www.aliexpress.us/item/3256810043215346.html) $3.38 / 10 | Amphenol **87180-062LF**. On AliExpress pick the **322** variant. Alternative: [54601-006](https://www.digikey.com/en/products/detail/amphenol-icc/54601-006/1158894) |
| | 1 | **J5** 2×4 pin header, 2.54 mm | [DigiKey](https://www.digikey.com/en/products/detail/same-sky-formerly-cui-devices/SSK03-MPH-254-B4/28772142) $0.13 | | **SSK03-MPH-254-B4**, or two 1×4 strips cut from any header stock. Fitted on every build |

## Second phone line · +about $10

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **U3** AG1171 SLIC (line 1, at **J8**) | same as **U4** | same as **U4** | |
| | 1 | **C17** 470 µF 16 V radial | same as **C16** | same as **C16** | |
| | 1 | **J8** RJ12 jack | same as **J9** | same as **J9** | |
| | 4 | **J5** jumper shunts, 2.54 mm | [DigiKey](https://www.digikey.com/en/products/detail/sullins-connector-solutions/QPC02SXGN-RC/2618262) $0.29 / 10 | | **QPC02SXGN-RC**. Bridge all four pairs: 1-2, 3-4, 5-6, 7-8 |

## Optional: DC jack and line protection

| | Qty | Component | DigiKey | AliExpress | Notes |
|---|-----|-----------|---------|------------|-------|
| | 1 | **J1** DC barrel jack, 5.5 × 2.1 mm | [DigiKey](https://www.digikey.com/en/products/detail/mpd-memory-protection-devices/EJ508A/2439547) $1.25 | [AliExpress](https://www.aliexpress.us/item/3256802445218613.html) $2.70 / 10 | The AliExpress jack is verified to fit. **EJ508A** should, but its pins haven't been checked against the footprint |
| | 1 | **D12** line TVS diode | [DigiKey](https://www.digikey.com/en/products/detail/taiwan-semiconductor-corporation/P6KE82A/1050320) $0.58 | [AliExpress](https://www.aliexpress.us/item/3256809218310441.html) $1.96 / 20 | **P6KE82A**, any maker. Get the **A** version: the plain P6KE82 breaks down too close to ringing voltage. |

### Power: either input, and 2A

USB-C and the DC jack are diode-ORed onto one 5 V rail, so use whichever is
convenient:

- **USB-C** into the DevKit's own port, from a **2A** charger.
- **DC jack**: a **2A**, 5.5 × 2.1 mm, centre-positive supply. It loses
  about 0.4 V through the OR diode, so a **5.5–6 V** supply is better than a
  bare 5 V one.

2A covers the worst case: two phones ringing at once while the Wi-Fi
transmits. One idle line needs far less, but an undersized supply browns out
mid-ring, which looks exactly like a random reboot.

Fit the DC jack if the board lives somewhere permanent; skip it if it will
always run from a USB charger. 

If using the 30-pin esp32-c6 cheaper variant, you will want the DC jack because the usb-C connection is inset from the edge of the board.

### Is the line TVS worth fitting?

**Recommended.** Normal operation never reaches it (it clamps at 113 V,
above anything the board produces). It protects the SLICs in two realistic
accidents:

- **Someone plugs the board into a real phone line.** The RJ12 jacks look
  exactly like wall jacks, and a live telco line has ring voltage of its own.
- **Surges from outside wiring**: lightning-induced transients and static
  arriving down a long phone cord.

If every phone sits in the same room on a short cord and the jacks can never
reach house wiring, you can leave it off. Otherwise, $0.58 protecting an
$8.50 SLIC is cheap insurance.

## Optional: hacker headers

None of these are needed for any feature. Generic 2.54 mm pin headers.

| Ref | What it is | What it's for |
|-----|-----------|---------------|
| **J7** | 1×3 audio monitor output | Mirrors the earpiece audio: scope it, feed a powered speaker, or listen in without picking up. Needs **C10** and **C11** fitted (below) |
| **J3**, **J4** | 1×2 each, GPIO16 and GPIO17 | Spare ESP32 pins: a button, an LED, a relay, or a serial port. Not available with the Bluetooth companion, which uses them during setup|

**C10/C11, for J7 only:** fit two **1 µF** non-polarized film or ceramic
capacitors in the 5 mm disc footprints. That passes the whole phone band.
To drive headphones directly, use 47 µF electrolytics instead, **+ toward
the codec** (U2) and − toward J7. Left empty, J7 carries no signal.

## Bluetooth companion · coming soon

Arrives with the v5 board: an 8-pin header (**J2**) on the main board and a
small companion board carrying a BM64 or BK3266 Bluetooth module. Parts and assembly
will be listed here once v5 is tested.

---

## Microcontroller module: 32-pin or 30-pin clone

The v5 board also takes a cheap 30-pin clone. They aren't equivalent:

| | **32-pin (recommended)** | **30-pin clone (v5 only)** |
|----|----|----|
| Part | Genuine ESP32-C6-DevKitC-1 | AliExpress clone, about $4 |
| Fitting | Drops straight in | Move two solder jumpers on back of board, **JP2** and **JP3** |
| USB flashing and console | Works | Works once the jumpers are set |
| Status | Known good | **Untested** |

<table>
  <tr>
    <td width="50%"><img src="pics/v6_esp32_solder_jumpers.png" alt="The Analog Redial board"></td>
    <td width="50%"><img src="../docs/assets/photo-placeholder.svg" alt=" (photo coming soon)"></td>
  </tr>
  <tr>
    <td align="center">Modification needed for cheap 30-pin esp32-c6-devkit</td>
    <td align="center">add photo of finished rework</td>
  </tr>
</table>

Use the genuine module unless you're happy to debug. Details in the
[v5 board reference](hardware-smd5.md#10-mcu-module-variants).

## Flash size: 16 MB or 8 MB

The firmware fits either. What changes is room for recordings on a board
without a microSD card:

| | 16 MB | 8 MB |
|---|---|---|
| Two firmware-update slots | 2 MB each | 2 MB each |
| Built-in audio | 2 MB | 1 MB |
| Recordings, prompts, greetings | 6 MB, recordings capped at 4 MB | 2.875 MB, recordings capped at 2 MB |

Nothing to choose by hand: the flashing script reads the flash size off the
chip and builds the right image ([step 3](flashing.md)).

---

**Next: [Assembly](building.md)**
