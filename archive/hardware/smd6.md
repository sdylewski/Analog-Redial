# SMD6 (Analog Redial v6): line 2 on an I²C expander

Working notes, not public docs. **The board isn't built, and this firmware has
never run on hardware.** The driver has only a host test against a simulated chip
(`firmware/test/host/test_line_expander.c`).

## How it's wired

U6 is a TCA9534APWR at 0x38 (A0-A2 to GND), on the same I²C bus as the ES8388 and J11.

| U6 pin | Port | Net | Firmware |
|---|---|---|---|
| 4 | P0 | Line1_F/R, R18 10 k to GND | output |
| 5 | P1 | Line1_RM, R19 10 k to GND | output |
| 6 | P2 | Line1_SHK, R13 10 k from U3, R12 100 k to GND | input |
| 7 | P3 | Line1_PD, through D2 to U3 | low = power down, input = run |
| 9-12 | P4-P7 | J10 | output, low |
| 13 | ~INT | not connected | polled instead |

GPIO18-21 go only to the microSD socket. There's no J5.

## Firmware

`CONFIG_BOARD_TYPE_SMD6` sets `BOARD_LINE1_ON_EXPANDER`. Only
`ag1170.c` touches SLIC pins, so the change there is one `on_expander(line)` test at
each pin access. `line_expander.c` does the rest:

- `ag1170.c`'s 1 ms monitor task never waits on I²C. It reads a cached SHK and writes
  wanted outputs to memory, and a separate task does the bus traffic. Line 0's timing
  doesn't change.
- SHK is read every 4 ms, about 0.4 ms on the bus at 100 kHz, so about 10 % bus use.
  Outputs go out as soon as they change.
- Every second it reads the config register back. The chip has no reset pin and can
  lose power alone, which puts it back to all inputs.
- 50 failed reads in a row (about 200 ms) make the line read on-hook, reset the bus
  and retry once a second.
- At boot it writes the output register before the config register, because the chip
  keeps its state through an ESP32 reset and a crash could have left RM high.
- `board_i2c_init()` clocks the bus free first on SMD6, since polling makes a reset
  mid-transfer likely enough to matter.

Not supported yet: pin outputs on J10 (`auxpins` reports none on this board), and using
`~INT`. BM64 EEPROM programming isn't possible on v6: RST_N and P2_0 have no route.

**No v6 OTA image:** `release/release.py` has no `smd6` entry, so it refuses one.
Add it there when v6 is tested.

## Numbers checked against the datasheets

- **No internal pull-ups** on the TCA9534A (TI SCPS198C, section 8.1), which is why
  R18 and R19 matter: all pins are inputs at power-up, and F/R and RM would otherwise float.
- **SHK levels:** off-hook about 2.9 V at P2 (3.3 V through 3.3 k + 10 k into 100 k).
  On-hook 0.45 V or less, against VIL 0.2 x VCC = 0.66 V. TI's table has no VIH row for
  P-ports at VCC of 3 V or more; 0.7 x VCC (2.31 V) from the row below 2.7 V is assumed.
- **F/R and RM drive:** the AG1171 table lists VIH 3.5 V at 5 V, and 2.0 V at 3.3 V.
  U6 gives about 3.3 V, the same as the ESP32 GPIOs on line 0, which work.

## Bring-up, in order

1. Before fitting U3, or with J8 empty: `i2c_scan` shows 0x10 and 0x38.
2. Unprogrammed ESP32 or held in reset: measure F/R and RM at U3. Both must be low. The
   AG1171 lists up to 0.5 mA input current, so R18 and R19 alone may not hold them firmly.
3. `line_exp` on the console: answering, SHK 0 on-hook, config 0x0C.
4. Lift line 2's handset: SHK 1. Dial pulses count correctly, compared with line 0.
5. Ring line 2 at 20 Hz, and at 66 Hz from the ring frequency test. Off-hook stops it.
6. **Audio noise:** listen on both lines with a quiet line. The bus now runs at 250 Hz
   all the time, so a 250 Hz tone means SCL or SDA couples into audio. If it does,
   raise `EXP_POLL_MS`; pulse dialing has room up to about 8 ms.
7. **Heap:** two lines plus microSD is a combination no earlier board could have. Check
   free heap on the Hardware page during a call with a card mounted.
8. Pull U6's power or short SCL briefly during a call: the line goes on-hook and recovers.

9. **Two lines on the card at once:** record a 211 on one line while a long clip plays on
   the other, and listen to both. FatFs's volume lock keeps this safe, but one writer's
   slow SD write holds up the other stream, and the audio buffers hold about 80 ms
   (`s_frame * 8` at 48 kHz). Past that, expect a click or a gap, not damage. If it's
   audible, make those buffers bigger.
10. **Coin board on the bus:** fire the relay repeatedly and watch `bus_errors` in
    `line_exp`. The relay's noise shares a cable with SCL and SDA.

## Sharing the bus with the coin board

U1 on the coin board is the same chip at 0x39, also polled every few ms, so reads cost
about 20 % of the bus together. Poll it from the task in `line_expander.c` rather than a
second task, and skip it while it isn't answering. `i2c_master_bus_reset()` takes no bus
lock, so a reset from a second task could land on another transfer.

## Known to come back to

Two lines recording in the same second get the same filename (`rec_name_timestamped()`,
`rec_name_voicemail()`, no line number). On SPIFFS that overwrites one recording. On the
card it's worse: `CONFIG_FATFS_FS_LOCK=0`, so the second writer unlinks and recreates a file
the first still has open, and both can write to the same clusters. Two lines plus microSD
makes this reachable for the first time on v6. Fix before v6 ships: add a suffix when the
other line already holds that name, or set `CONFIG_FATFS_FS_LOCK` to 8 or more so FatFs
refuses the second open.

The AG1171 datasheet says F/R logic low reverses polarity, and asks for F/R high for
more than 10 ms before RM goes low when ringing ends. `ag1170.c` idles F/R low and drops
RM first, on every board. Lines work like this, so nothing changed for v6.
