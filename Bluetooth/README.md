# Bluetooth modules for the J2 header

What has been tried for the hands-free "rotary phone as a cell-phone handset" option. Newest information first.

| Module | Price | Status |
|---|---|---|
| **Microchip BM64** (carrier board in `BM64/`) | about $14 | **Works and is supported.** UART link, pairing mode and the audio EEPROM setup verified (2026-10-20); call audio and real phone pairing still to test with a handset. Supply 3.2-4.2 V on `BAT_IN`, so it needs the LDO. |
| **DFRobot DFR0781** (BT401 module, `DFR0781_BT401/`) | about $3.50 | **Works, and now the preferred option** (2026-10-27, hand-wired to a v5 board): pairing, music, ringing, dial-out and call audio, with little call delay. Text command set with call control (answer, hang up, reject, dial, caller number); no EEPROM setup step. No adapter board yet. Pin table in `DFR0781_BT401/BT401_NOTES.md`, firmware and bench log in `../archive/hardware/bt401.md`. |
| Microchip BM83 | about $12.20 | Candidate only. Digikey lists BM83SM1-00AA, -00AB and -00TA (read the Microchip ordering table before buying). Same 0xAA binary framing as the BM64 per Microchip's UART command documentation, so the driver would probably adapt, but footprint and pinout are not confirmed to match; needs its own carrier. Not worth the $1.80 saving unless the BM64 becomes hard to get. |
| **BK3266 / "BH519"** (`BK3266/`, `BK3266_board_v1` KiCad project) | about $3 | **Not working as intended.** The module powers on and pairs with a phone as an audio device ("BH519"), but its UART never answers (binary or text, 1200-460800 baud, TX/RX both ways), so the firmware cannot control calls. Notes and bench log: `BK3266/BK3266_RESEARCH.md` and `../archive/hardware/bk3266.md`. Kept for reference only; its firmware driver was removed (last in `v1.0.1`). |

Firmware lives in `firmware/components/bt_companion/` (the BM64 and BT401 drivers). The public docs describe the BM64 and mention the BT401 as the cheaper option.
