# Analog Redial v4 (SMD4) — dev notes

The current board: 2-layer, ESP32-C6-DevKitC-1 carrier, ES8388 codec, one or
two AG1171 SLICs, optional microSD. KiCad project:
`hardware/kicad/Analog Redial v4/`; firmware `CONFIG_BOARD_TYPE_SMD4`,
`board_pins_smd4.h`. The public build reference is
[`docs/hardware-smd4.md`](../../docs/hardware-smd4.md); these are the
internal notes behind it.

(v3, `hardware/kicad/Old/Analog Redial v3/`, is the same schematic laid out
on 4 layers. It was never made.)

## Pin map (ESP32-C6-DevKitC-1, U1)

From the schematic netlist. The two SLICs are named three ways depending on
the net group — go by this table, not a net name on its own:

| Ref | Firmware line | RJ12 | Control nets | Tip/Ring nets |
|--|--|--|--|--|
| U4 | **line 0** | J9 | `/Line0_*` | `/Line1*` |
| U3 | **line 1** | J8 | `/Line1_*` | `/Line2*` |

| GPIO | Signal | Notes |
|--|--|--|
| 0 | I2S DIN | from codec `ASDOUT` |
| 1 | I2S LRCK | |
| 2 | I2S MCLK | |
| 3 | microSD card-detect (`Card1` pin 9) | internal pull-up; low = card present |
| 4 | Line 0 F/R | strapping pin, driven as an output — safe |
| 5 | Line 0 RM | strapping pin, as above |
| 6 | Line 0 SHK | via the R15/R14 divider |
| 7 | Line 0 PD | via D3 (diode only, per AG1171 datasheet §3.4) |
| 8 | DevKit's onboard RGB status LED | strapping pin |
| 9 | DevKit BOOT button | download-mode strap; firmware reads it at runtime (3 s hold = setup network, 10 s = forget Wi-Fi) |
| 10 | I2S DOUT | to codec `DSDIN` |
| 11 | I2S BCLK | to codec `SCLK` |
| 12, 13 | native USB D−/D+ | flash and console |
| 15 | free | strapping (JTAG source select, eFuse-gated) |
| 16, 17 | UART0 TX/RX | J3/J4 spare headers |
| 18–21 | microSD **or** line 1 | selected by J5, below |
| 22, 23 | I2C SDA/SCL | codec control |

### J5: line 1 or microSD

GPIO18–21 go to the microSD socket directly; line 1's SLIC control signals
come out on the other pin of each J5 pair (2×4, 0.1"):

| J5 | Signal | J5 | Signal |
|--|--|--|--|
| 1 | GPIO21 (SD CS) | 2 | Line 1 PD |
| 3 | GPIO20 (SD MOSI) | 4 | Line 1 RM |
| 5 | GPIO19 (SD CLK) | 6 | Line 1 SHK |
| 7 | GPIO18 (SD MISO) | 8 | Line 1 F/R |

Four shunts (1–2, 3–4, 5–6, 7–8) for two lines; none for microSD. Without
shunts line 1 has no GPIO at all. Firmware's Settings → Hardware choice
must match.

## Power

USB-C on the DevKit, or the DC jack J1 through **D1** (SS14 Schottky,
cathode to +5 V) — diode-OR'd onto the +5 V net that feeds the SLICs'
`Vpwr`, so neither source can back-feed the other. The 3.3 V rail for the
ES8388 and the microSD comes from the DevKit's own 3.3 V pin. D1 drops
~0.3–0.5 V, so a DC supply should be 5.5–6 V rather than exactly 5 V.

## SHK divider rework (every built board)

Built with 10 kΩ / 15 kΩ dividers (R15/R14 line 0, R13/R12 line 1). The
AG1171's SHK output is ~3.3 V through an internal 3.3 kΩ pull-up, so with
15 kΩ off-hook reads ~1.9 V — under the C6's 2.475 V guaranteed input-high.
Which line fails is luck of the pad threshold.

**Remove R12 and R14 (the 15 kΩ to ground).** Off-hook then reads
~3.1–3.2 V and the 10 kΩ stays as series protection. (SMD5 uses 100 kΩ.)

## Expected ERC/DRC noise

- **"PD not driven" / "EN not driven"**: PD is diode-driven on purpose, and
  the DevKit has its own EN pull-up and reset button. Not real.
- **Footprint/symbol library warnings on JLC-imported parts** (diodes,
  `Card1`, RJ12s): `JLCImport` isn't in the global library tables. The
  project-local `fp-lib-table`/`sym-lib-table` and `JLCImport.*` are
  git-ignored (machine-specific paths), so a fresh checkout needs the
  shared library repo linked in `docs/hardware-smd4.md`.
- Silkscreen-over-copper and silkscreen-overlap warnings: cosmetic.

## 2-layer notes

No inner planes, so GND is a poured zone with stitching vias. When editing
the layout, check the pour is continuous under the audio path (SLIC Vin/Vout,
ES8388 analog pins) and that the +5 V run to the SLICs' `Vpwr` is wide
enough. Nothing here is fast enough (I2S at audio rates) to need more.

## ESP32-C6 GPIO constraints

From ESP-IDF's C6 GPIO table:

- **Strapping pins: 4, 5, 8, 9, 15.** Fine as I/O after boot, as long as
  nothing holds them at the wrong level during reset. GPIO9 (download
  mode) is the one that matters.
- **12/13** are native USB on this board — don't reuse them.
- **24–30** are internal flash; **14** isn't brought out on the module.

## See also

- [`smd2.md`](smd2.md) — the previous board (three built).
- [`smd5.md`](smd5.md) — the next board: dual MCU footprint and Bluetooth.
