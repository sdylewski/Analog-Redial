# DFRobot DFR0781 (BT401 module): notes

**Part number to order: DFR0781** (DFRobot's SKU; Digikey lists it under that number, about $3.50). The module itself is the BT401. Product page: https://www.dfrobot.com/product-2177.html . Wiki: https://wiki.dfrobot.com/dfr0781/ . Datasheet (translated, saved here): `DFR0781_BT401_datasheet_english-translated.pdf`.

2026-10-20, from DFRobot's translated datasheet (BT401 V1.5).
Modules received 2026-10-27; first power-up not done yet. Why it is interesting: a **text** UART protocol with the call
control we need, at about $3.50. (How it compares with the other modules: `../README.md`.)

- Bluetooth 5.0 dual mode; HFP/HSP/A2DP/AVRCP/SPP/BLE. Jieli-style chip (default name "BT201-AUDIO").
- UART 3.3 V TTL, **115200 8N1** by default (changeable, `AT+CT05`), commands `AT+<cmd>[param]\r\n`,
  replies `<IND>[param]\r\n`. VCC 3.3-5.2 V, about 20 mA. Stamp-hole module, 23 x 15.5 mm, 26 edge pads plus 4 round pads (27-30).
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
- Firmware: driver `firmware/components/bt_companion/bt401.c` (notes and bench log in `archive/hardware/bt401.md`).
  Working on a v5 board since 2026-10-27: music, incoming calls ringing, dial-out, call audio. The driver
  switches the module's DAC off (`AT+CS00`, high-impedance) when there's no Bluetooth audio, because an
  idle DAC loads line 0's earpiece input through R16; with that, no series resistor is needed (J2.7 wired
  straight to pad 10).

## Adapter board for the J2 header (BM64 style)

Reference: DFRobot's schematic `DFR0781_bt401-module_schematics_V1.0.pdf` (the "DAC output" version) and
datasheet page 41 (footprint). Bookmarks in `LINKS.md`. Module pad numbers below are the BT401's own.

| J2 pin | Main board signal | BT401 pad | BT401 name |
|---|---|---|---|
| 1 | `+3.3V_BM` (U5) | 13 | VCC |
| 2 | GPIO17 (ESP receives) | 26 | PB0, UART TX |
| 3 | GPIO16 (ESP transmits) | 25 | PB1, UART RX |
| 4 | GPIO15 / MFB | none | not needed |
| 5 | Line 0 Vout, via C22 1 uF | 8 | PA0, MIC |
| 6 | GND via C24 1 uF (`MIC_N`) | none | unused |
| 7 | Line 0 Vin, via C23 1 uF and R16 1 kohm | 10 | DACL |
| 8 | GND | 12 and 11 | GND, SGND |

Notes per pin:

- **J2.1, VCC (pad 13):** 3.3-5.2 V accepted; the vendor says 4.2 V is best, and 5 V through a diode is fine. A 3.7 V U5 suits it. Do **not** power pad 6: 3V3 is the module's own LDO **output**, 80 mA max.
- **J2.2, module TX (pad 26):** 3.3 V level, 115200 8N1 by default.
- **J2.3, module RX (pad 25):** a 1 kohm series resistor is only needed for 5 V hosts.
- **J2.4, GPIO15 / MFB:** not needed (R17 pulls it low). Optional later: pad 18 (PR2, "ENABLE / RESET") if a reset line is wanted.
- **J2.5, MIC (pad 8):** the call microphone pin, fixed. The vendor biases it from pad 7 (DACVDD) through 6.2 kohm and couples with 1 uF; copy that: 6.2 kohm from pad 7 to pad 8, 1 uF in series (C22 on the main board already is one). It expects an electret microphone level, so check the level.
- **J2.6, `MIC_N`:** unused, the module's mic input is single-ended; leave it unconnected.
- **J2.7, DACL (pad 10):** left-channel speaker/headphone output (a headphone amplifier). The vendor adds 22 ohm + 10 uF for headphones; we already have C23 + R16. DACR (pad 9) stays open.
- **J2.8, GND (pads 12 and 11):** join analog (SGND, pad 11) and digital (GND, pad 12) ground **at the supply entry point only**, one star point (the vendor stresses this).

Leave everything else unconnected; the vendor says to leave unused pins floating, no pull-ups or pull-downs:
pads 1-5 (AUX, ADKEY, USB), 14-24 (GPIO, TF card, MUTE, LED), 27-30 (four round pads the JLCPCB part names GND, TX, RX, 5V: apparently a separate serial/test header, not the antenna; the module has its own PCB antenna, nothing to connect; confirm on a real module). Pad 19 (PR1, LED, high = on, through 1 kohm) and pad 14 (PC1, BLE LED) are handy for a status
LED. Pad 23 (PB3, MUTE) auto-detects a pull-up/pull-down at power-up: leave it open.

Layout: antenna keep-out. The PCB antenna is the right-hand end of the module in the vendor drawing (pads 27/28
side): let that end overhang the board edge or leave it free of copper and metal on every layer.

Footprint (from the datasheet drawing, **verify on a real module before drawing the board**; KiCad symbol `Custom:DFR0781_BT401` and footprint `Custom:DFR0781_BT401` with a 3D model are in the Custom library, imported from JLCPCB part C7265136 because DFRobot publishes no CAD files): body 23.0 x 15.5 mm;
stamp-hole (castellated) pads 0.8 mm wide on a 1.27 mm pitch; pads 1-13 along one long edge with pad 1 at the antenna
end, pads 14-26 along the opposite edge starting at the end away from the antenna, the drawing shows four larger round holes on the left short edge
(unlabeled; the JLCPCB footprint names them 27-30 GND, TX, RX, 5V). Dimension labels in the drawing: 7.2 mm from the antenna-end edge to pad 1,
15.5 mm board width, 17.5 mm across the pad tips.

Software: AT text protocol, replies end in CRLF. Answer `AT+BA04`, hang up `AT+BA03`, reject `AT+BA02`, call back
`AT+BA00`, dial `AT+BT<number>` (reply `OK`), status `AT+TS` (`TS+00` .. `TS+04`), caller number `AT+TT` or the unsolicited
`TT+<number>` every 2 s while ringing. Confirmed against the code in `DFRobot_BT401/DFRobot_BT401.cpp`.
Enable calls with `AT+B201`.

## Bench test: first power-up (UART only, no audio parts)

Four wires are enough to see whether the module talks. Do this before building the audio path.

| BT401 pad | Connect to | Note |
|---|---|---|
| 13 VCC | 3.3 V (J2.1, or the DevKit 3V3 pin) | pad 6 stays unconnected |
| 12 GND and 11 SGND | GND (J2.8) | tie 11 and 12 together |
| 26 TX | GPIO17 (J2.2) | module TX to ESP RX |
| 25 RX | GPIO16 (J2.3) | ESP TX to module RX |

Leave every other pad open, and keep the antenna end clear of metal and your hand.

1. Flash firmware with the BT401 driver; on the USB console run `hwcfg bt bt401`, then `reboot`.
2. `bt_enable 0` (the probe needs the driver stopped), then `bt401_selftest` (no module needed: `PASS`).
3. Power-cycle the module, then `bt401_probe`. Expect `QA+30` / `QM+00` within 2 s, then replies to `TS`, `TD`, `TA`. Silence means power, swapped TX/RX, or another baud rate.
4. `bt401_at B201` (enable calls), `bt401_at BA06` (discoverable), pair a phone with "BT201-AUDIO".
5. `bt_enable 1`, `bt401_state` (connected should read 1), then call the phone and watch the `rx: status 3` lines.

Write what happened into the Log in `archive/hardware/bt401.md`.
