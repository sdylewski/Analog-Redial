# Bluetooth modules for the J2 header

What has been tried for the hands-free "rotary phone as a cell-phone handset" option. Newest information first.

| Module | Price | Status |
|--|--|--|
| **Microchip BM64** (carrier board in `BM64/`) | about $14 | **Works and is supported.** UART link, pairing mode and the audio EEPROM setup verified (2026-10-20); call audio and real phone pairing still to test with a handset. Supply 3.2-4.2 V on `BAT_IN`, so it needs the LDO. |
| **DFRobot DFR0781** (BT401 module, `DFR0781_BT401/`) | about $3.50 | **Next best option, not yet tested.** Text command set with call control (answer, hang up, reject, dial, caller number). To be hand-wired to J2 first; pin table in `DFR0781_BT401/BT401_NOTES.md`. |
| Microchip BM83 | about $12.20 | Candidate only. Same style of binary protocol as the BM64; footprint not confirmed to match; needs its own carrier. |
| **BK3266 / "BH519"** (`BK3266/`, `BK3266_board_v1` KiCad project) | about $3 | **Not working as intended.** The module powers on and pairs with a phone as an audio device ("BH519"), but its UART never answers (binary or text, 1200-460800 baud, TX/RX both ways), so the firmware cannot control calls. Notes and bench log: `BK3266/BK3266_RESEARCH.md` and `../archive/hardware/bk3266.md`. Kept for reference only. |

Firmware lives in `firmware/components/bt_companion/` (BM64 driver; the BK3266 driver and its console commands are still there). The public docs describe only the BM64.
