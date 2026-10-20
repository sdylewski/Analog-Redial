# Bill of Materials — Bluetooth companion (optional)

Parts for the optional **BM64 Bluetooth Classic audio companion**: the
carrier board (`Bluetooth/Bluetooth_board/`) plus the parts that exist on
the main board *only* because of this option. Everything here is optional —
a board built without Bluetooth needs none of it.

Design reference: [`smd5.md`](smd5.md) (J2
header pinout, MFB, LDO) and
[`../BLUETOOTH_AUDIO_UPGRADE.md`](../BLUETOOTH_AUDIO_UPGRADE.md) (why each
coupling cap and the series resistor are the values they are).

**U5 on the next board (v6):** the 3.3 V XC6206P332MR becomes the 3.6 V XC6206P362MR, to support a wider range of future Bluetooth modules (the schematic already has it). v5 stays as built.

**Prices last checked 2026-09-30.** DigiKey prices can be re-read
automatically from the product pages; **AliExpress and Amazon block
automated fetches**, so those prices have to be pasted in by hand when they
change. Blank cell = no link sourced for that store yet, not "unavailable".

---

## Carrier board (`Bluetooth/Bluetooth_board/`)

**DigiKey subtotal: ~$14.80** (the 4-pin programming header isn't sourced yet).

| Qty | Ref | Part number | Description | DigiKey | AliExpress | Amazon |
|-----|-----|-------------|-------------|---------|------------|--------|
| 1 | **U5** | `BM64SPKS1MC2-0003AA` | BM64 Bluetooth 5.0 stereo audio module, Class 2, RF shield, 43-SMD | [DigiKey](https://www.digikey.com/en/products/detail/microchip-technology/BM64SPKS1MC2-0003AA/13252265) $14.40 | | |
| 1 | **J1** | `SSK02-FPH-254-A8` | 8-pin female header, 2.54 mm — mates with the main board's J2 | [DigiKey](https://www.digikey.com/en/products/detail/same-sky-formerly-cui-devices/SSK02-FPH-254-A8/28772140) $0.40 | [AliExpress](https://www.aliexpress.us/item/2255801012106911.html) $1.71 / 10 pcs | [Amazon](https://www.amazon.com/120pcs-2-54mm-Connector-Assembly-Adaptor/dp/B0DB5LV11C/) $4.99 / 120 pcs kit |
| 1 | **J2** | — | 4-pin male header, 2.54 mm — one-time EEPROM programming header (P2_0 / RST_N / MFB), mates to J5 | | | |

**Not in the carrier schematic yet** (recommended in
`smd5.md`, add before ordering): **10 µF + 100 nF** decoupling
at the module's `BAT_IN` pin. Both are ordinary 0402/0603 ceramics already
in the main board's reel.

---

## Main board (SMD5) — parts added for the Bluetooth option

Skip this whole table on a board built without Bluetooth.

**Check the LDO's input rating against your DC supply.** The XC6206's
maximum input is around 6 V, and the board's +5 V rail sits only a diode drop
below whatever the DC jack is fed — so a 6 V brick leaves little margin. Fine
on USB or a 5 V supply; confirm against the datasheet if you run the jack at
the top of the recommended 5.5–6 V range.

**DigiKey subtotal: ~$0.17** — everything else here is stock 0402 passives
and a sub-$0.10 LDO, so the Bluetooth option lands at **~$15 all in**.

| Qty | Ref | Part number | Description | DigiKey | AliExpress | Amazon |
|-----|-----|-------------|-------------|---------|------------|--------|
| 1 | **J2** | `SSK01-MPH-254-A8` | 8-pin male header, 2.54 mm — the companion link (power, both audio directions, UART, MFB) | [DigiKey](https://www.digikey.com/en/products/detail/same-sky-formerly-cui-devices/SSK01-MPH-254-A8/28772164) $0.17 | | |
| 1 | **U5** | `XC6206P332MR-G` | 3.3 V LDO, SOT-23-3L — dedicated BM64 rail off +5 V, keeps the module off the DevKit's regulator. **LCSC `C5446`, a JLC Basic part at ~$0.14** (verified 2026-09-30), so no Extended fee | | | |
| 2 | **C25, C26** | — | 1 µF 0402 ceramic, ≥6.3 V — LDO input and output | | | |
| 1 | **C23** | — | 1 µF 0402 ceramic, ≥6.3 V — AOHPL → Line0Vin coupling | | | |
| 1 | **C22** | — | 1 µF 0402 ceramic — Line0Vout → MIC_P1 coupling | | | |
| 1 | **C24** | — | 1 µF 0402 ceramic — MIC_N1 AC ground (differential input's other half) | | | |
| 1 | **R16** | — | 1 kΩ 0402 — series into Line0Vin, limits fault/inrush current into the SLIC | | | |
| 1 | **R17** | — | 100 kΩ 0402 — MFB pull-down, holds it low while the ESP32 is in reset | | | |

The passives are stock 0402 values already used elsewhere on the board — no
separate line item if they're coming from JLC's basic library.

---

## Which BM64 part number to order

DigiKey lists several suffixes on the same `BM64SPKS1MC2` base part. The
datasheet's own ordering table (§10) stops at the base number and doesn't
document the suffixes at all — they're factory firmware/config variants.

| Suffix | What's known | Use it? |
|--------|--------------|---------|
| **-0003AA** | DSPK **2.1.5** firmware (SweynTooth and BIAS fixes). **This is the one this project has tested.** | **Yes** |
| -0002AA | DSPK **2.1.2** firmware plus a Bluetooth SIG BR/EDR security patch. Same price, same specs otherwise. | No |
| -00M3AA | Listed by distributors as the same module; the suffix's meaning could not be confirmed from Microchip documentation or distributor pages. | No |

**Order `-0003AA`.** This isn't only "it's the one that worked" — the EEPROM
byte addresses this project writes (`0x01FC`, `0x0094`, `0x0171`, `0x01D7`,
`0x01D8`, `0x0348`, in `bt_companion.c`'s `s_expected_config[]`) were read
out of the **DSPK v2.1.5** parameter table. A module that ships with
different factory firmware may lay those bytes out differently, and the
symptom would be `bm64_config` reporting unexpected values — or, worse,
writing to the wrong addresses. If you ever do buy a different suffix,
re-derive the addresses from that firmware's own parameter table before
running the config check against it.

Stock/lead time as of 2026-09-30: 85 pcs at DigiKey for `-0003AA`, 720 pcs
for `-0002AA`, both with a 20-week manufacturer lead time — worth ordering a
spare, since a re-order is not quick.
