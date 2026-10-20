# DFRobot DFR0781 (BT401 module) as a cheap hands-free module: candidate notes

**Part number to order: DFR0781** (DFRobot's SKU; Digikey lists it under that number, about $3.50). The module itself is the BT401. Product page: https://www.dfrobot.com/product-2177.html . Wiki: https://wiki.dfrobot.com/dfr0781/ . Datasheet (translated, saved here): `DFR0781_BT401_datasheet_english-translated.pdf` (a plain-text extraction sits next to it).

2026-10-20, from DFRobot's translated datasheet (BT401 V1.5).
Not bought or tested. Why it is interesting: a **text** UART protocol with the call control we need,
and it costs about $3.50, against about $12-14 for a BM64 / BM83.

- Bluetooth 5.0 dual mode; HFP/HSP/A2DP/AVRCP/SPP/BLE. Jieli-style chip (default name "BT201-AUDIO").
- UART 3.3 V TTL, **115200 8N1** by default (changeable, `AT+CT05`), commands `AT+<cmd>[param]\r\n`,
  replies `<IND>[param]\r\n`. VCC 3.3-5.2 V, about 20 mA. Stamp-hole module, 23 x 15.5 mm, 28 pads.
- Call control: `AT+BA04` answer, `AT+BA03` hang up, `AT+BA02` reject, `AT+BA00` call back the last
  number, `AT+BT<number>` **dial a number**. Events: `TS+00` waiting/not connected, `TS+01` connected idle,
  `TS+02` playing music, `TS+03` incoming or outgoing call ringing, `TS+04` in a call;
  `TT+<number>` the other party's number, repeated every 2 s until the call connects or ends.
  Switch: `AT+B201` enables calls. `AT+TS` / `AT+TT` query.
- Audio: 16-bit stereo DAC with headphone amplifier (DACL pin 10, DACR pin 9, SGND pin 11);
  call microphone is the fixed pin 8 (PA0), electret-style with an external capacitor.
- Pins for our J2 header (8-pin): module pad 13 VCC <- J2.1 (+3.3 V_BM); pads 12 and 11 GND <- J2.8;
  pad 26 (module TX) -> J2.2 (GPIO17); pad 25 (module RX) <- J2.3 (GPIO16); pad 8 MIC <- J2.5
  (line 0 Vout through 1 uF); pad 10 DACL -> J2.7 (through 1 uF and 1 k to line 0 Vin). MFB (J2.4,
  GPIO15) is not needed. A level check is needed: the mic input expects electret level, the DAC
  is headphone level.
- Needs: a new driver (like bk3266.c but line-oriented text), a small carrier or wires for the 28-pad
  stamp module, bench check of call audio quality both ways.

## Other candidate: Microchip BM83 (about $12.20 on Digikey, against about $14 for the BM64)

Digikey lists three variants: **BM83SM1-00AA, BM83SM1-00AB and BM83SM1-00TA** (what separates them
was not checked: read the Microchip datasheet's ordering table before buying). Same 0xAA binary framing
family as the BM64 per Microchip's UART command documentation, so the existing driver would probably adapt,
but the footprint and pinout were not confirmed to match the BM64's: treat it as a different module that
needs its own carrier board. Not worth the $1.80 saving unless the BM64 becomes hard to get.

## Other candidate (not for hands-free): BH519 / BK3266-style module

See `../BK3266/` and `archive/hardware/bk3266.md`: powers on and pairs but its UART never answered.
