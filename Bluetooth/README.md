# Bluetooth modules for the J2 header

What has been tried for the hands-free "rotary phone as a cell-phone handset" option. Newest information first.

| Module | Price | Status |
|---|---|---|
| **DFRobot DFR0781** (BT401 module, `DFR0781_BT401/`) | about $3.50 | **Preferred, and working** (hand-wired to a v5 board 2026-10-27, then on the adapter board): pairing, music, ringing, dial-out and call audio, with little call delay. Text command set with call control (answer, hang up, reject, dial, caller number); no EEPROM setup step. Adapter board `DFR0781_BT401/DFR0781_BT401_board_v1/` plugs into J2 like the BM64 carrier (built, 2026-11-19; Gerbers via KiCad's JLCPCB/PCBWay export); a v2 is in `DFR0781_BT401_board_v2/`. Build guide: [`docs/bt401.md`](../docs/bt401.md). Pin table in `DFR0781_BT401/BT401_NOTES.md`, firmware and bench log in `../archive/hardware/bt401.md`. |
| **Microchip BM64** (carrier board in `BM64/`) | about $14 | **Works and is supported**, the older alternative. UART link, pairing mode and the audio EEPROM setup verified (2026-10-20). Supply 3.2-4.2 V on `BAT_IN`, so it needs the LDO. Costs more and needs a one-time EEPROM setup that the BT401 doesn't. |
| Microchip BM83 | about $12.20 | Candidate only. Digikey lists BM83SM1-00AA, -00AB and -00TA (read the Microchip ordering table before buying). Same 0xAA binary framing as the BM64 per Microchip's UART command documentation, so the driver would probably adapt, but footprint and pinout are not confirmed to match; needs its own carrier. Not worth the $1.80 saving unless the BM64 becomes hard to get. |
| **BK3266 / "BH519"** (`BK3266_do_not_use/`, `BK3266_board_v1` KiCad project) | about $3 | **Not working as intended.** The module powers on and pairs with a phone as an audio device ("BH519"), but its UART never answers (binary or text, 1200-460800 baud, TX/RX both ways), so the firmware cannot control calls. Notes and bench log: `BK3266_do_not_use/BK3266_RESEARCH.md` and `../archive/hardware/bk3266.md`. Kept for reference only; its firmware driver was removed (last in `v1.0.1`). |

Firmware lives in `firmware/components/bt_companion/` (the BM64 and BT401 drivers). The public docs lead with the BT401 ([`docs/bt401.md`](../docs/bt401.md), [`docs/bom.md`](../docs/bom.md#bluetooth-board-v5-and-v6-expansion-header-optional)) and describe the BM64 as the alternative.
