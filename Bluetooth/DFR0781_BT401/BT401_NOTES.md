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

## Adapter board for the J2 header (BM64 / BK3266 style)

Reference: DFRobot's schematic `DFR0781_bt401-module_schematics_V1.0.pdf` (the "DAC output" version) and
datasheet page 41 (footprint). Bookmarks in `LINKS.md`. Module pad numbers below are the BT401's own.

| J2 pin | Main board net | BT401 pad | Name | Notes |
|--|--|--|--|--|
| 1 | +3.3V_BM (U5) | 13 | VCC | 3.3-5.2 V accepted; the vendor says 4.2 V is best, and 5 V through a diode is fine. A 3.7 V U5 suits it. Do **not** power pad 6 (3V3 is the module's own LDO **output**, 80 mA max) |
| 2 | GPIO17 (ESP receives) | 26 | PB0 / UART TX | Module TX, 3.3 V level, 115200 8N1 by default |
| 3 | GPIO16 (ESP transmits) | 25 | PB1 / UART RX | Module RX; a 1 kohm series resistor is only for 5 V hosts |
| 4 | GPIO15 / MFB (R17 pulls it low) | none | | Not needed. Optional later: pad 18 (PR2, "ENABLE / RESET") if a reset line is wanted |
| 5 | Line 0 Vout through C22 1 uF | 8 | PA0 / MIC | The call microphone pin, fixed. The vendor biases it from pad 7 (DACVDD) through 6.2 kohm and couples with 1 uF; copy that: 6.2 kohm from pad 7 to pad 8, 1 uF series (C22 on the main board already is one). Check the level: it expects an electret microphone |
| 6 | GND through C24 1 uF (MIC_N) | none | | Unused: the module's mic input is single-ended; leave this unconnected |
| 7 | to Line 0 Vin through C23 1 uF and R16 1 kohm | 10 | DACL | Left channel speaker/headphone output (a headphone amplifier). Vendor adds 22 ohm + 10 uF for headphones; we already have C23 + R16. DACR (pad 9) left open |
| 8 | GND | 12 and 11 | GND, SGND | Join analog (SGND, pad 11) and digital (GND, pad 12) ground **at the supply entry point only**, one star point (the vendor stresses this) |

Leave everything else unconnected; the vendor says to leave unused pins floating, no pull-ups or pull-downs:
pads 1-5 (AUX, ADKEY, USB), 14-24 (GPIO, TF card, MUTE, LED), 27/28 (antenna: the module has its own PCB antenna,
nothing to connect). Pad 19 (PR1, LED, high = on, through 1 kohm) and pad 14 (PC1, BLE LED) are handy for a status
LED. Pad 23 (PB3, MUTE) auto-detects a pull-up/pull-down at power-up: leave it open.

Layout: antenna keep-out. The PCB antenna is the right-hand end of the module in the vendor drawing (pads 27/28
side): let that end overhang the board edge or leave it free of copper and metal on every layer.

Footprint (from the datasheet drawing, **verify on a real module before drawing the board**): body 23.0 x 15.5 mm;
stamp-hole (castellated) pads 0.8 mm wide on a 1.27 mm pitch; pads 1-13 along one long edge with pad 1 at the antenna
end, pads 14-26 along the opposite edge starting at the end away from the antenna, pads 27 and 28 on that bottom
edge at the antenna end, 4.9 mm past pad 26; the drawing shows four larger round holes on the left short edge
(unlabeled, probably USB/test). Dimension labels in the drawing: 7.2 mm from the antenna-end edge to pad 1,
15.5 mm board width, 17.5 mm across the pad tips.

Software: AT text protocol, replies end in CRLF. Answer `AT+BA04`, hang up `AT+BA03`, reject `AT+BA02`, call back
`AT+BA00`, dial `AT+BT<number>` (reply `OK`), status `AT+TS` (`TS+00` .. `TS+04`), caller number `AT+TT` or the unsolicited
`TT+<number>` every 2 s while ringing. Confirmed against the code in `DFRobot_BT401/DFRobot_BT401.cpp`.
Enable calls with `AT+B201`.
