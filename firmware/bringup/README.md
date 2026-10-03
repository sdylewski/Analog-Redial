# Bring-up guide

Hardware bring-up for the ESP32-C6 rotary phone gateway, covering both the
through-hole board (WM8731 codec) and the SMD boards (ES8388 codec). Two
layers:

1. **On-target console** (`firmware/components/bringup`) — a serial REPL
   built into the firmware itself, with one command per subsystem
   (`codec_init`, `tone`, `slic`, `ring`, `hookmon`, ...). This is what
   actually talks to the hardware.
2. **Host-side scripts** (this folder) — drive that console over serial so
   each subsystem can be tested with one command from your laptop, and so the
   full sequence can be scripted instead of typed by hand.

## first_setup.py — bring up and provision a board, one gated step at a time

Run it from an ESP-IDF shell (`. ~/esp/esp-idf/export.sh`), with the board on
USB and, for the HTTP steps, on Wi-Fi. Close any `idf.py monitor` first — it
holds the serial port.

```bash
./first_setup.py --host telephono.local                 # the usual run
./first_setup.py --host telephono.local --lines 2 --language de
./first_setup.py --chip-info --only chip                # chip, flash size, MAC only
./first_setup.py --host telephono.local --only audio verify
```

Each step runs only if the steps it depends on passed, and the run ends with
a PASS / WARN / FAIL / SKIP table:

| Step | What it does | Gates |
|------|--------------|-------|
| `chip` | *(opt-in, `--chip-info`)* esptool reads chip, flash size, MAC — no firmware needed; warns if flash isn't 16 MB | — |
| `console` | serial REPL answers; reads the board type the firmware was built for | `codec`, `microsd` test, `tests` |
| `codec` | ES8388 / WM8731 found on I²C | `tests` |
| `network` | board answers `/api/status` at `--host` | everything below |
| `hardware` | sets lines / microSD / Bluetooth — only when you pass `--lines` or `--bluetooth` | — |
| `microsd` | skipped on 2-line boards (shared pins); WARN if no card is in use; otherwise `sd_test` write/read/verify over serial | — |
| `audio` | generates and uploads `operator.wav`, `record_prompt.wav` (dial 211), `am_greeting_l<N>.wav` per line ("Leave a message after the beep"), and with `--digits`, `digit_0..9.wav` | `verify` |
| `verify` | reads every uploaded file back and checks it's a WAV | — |
| `tests` | *(opt-in, `--tests`)* the interactive `bringup_checklist.py` | — |

The operator message is assembled from what the board actually is: a
single-line board isn't told to dial 321, and the Bluetooth sentence only
appears when Bluetooth is fitted. Prompts go to internal flash, so they don't
depend on a card.

HTTP goes through `curl`: on macOS, Python's sockets to a LAN address are
blocked unless the terminal has Local Network permission. Speech is
generated locally by `gen_prompt.py`.

## 1. Build and flash

Board type is a compile-time Kconfig choice — GPIO routing is fixed per PCB,
so this can't be selected at runtime. Pick the one matching your board:

```bash
cd firmware
idf.py set-target esp32c6

# SMD boards (SMD2, SMD3 once routed) -- ES8388 codec:
idf.py -D "CONFIG_BOARD_TYPE_SMD2=y" build flash monitor

# Through-hole gateway -- WM8731 codec:
idf.py -D "CONFIG_BOARD_TYPE_THT=y" build flash monitor
```

**SMD boards also need two more Kconfig settings**, both easy to miss since they're
separate `choice`s from the board type above:

| Setting | Value for a dual-line SMD board | Why |
|---|---|---|
| `Component config -> ESP System Settings -> Channel for console output` | **USB Serial/JTAG Controller** | SMD boards have no UART bridge chip -- the default (UART0) reads keystrokes from GPIO16/17, which aren't wired to anything the host can talk to. Symptom if wrong: boot log prints fine, but the console never responds to typed input at all (UART0 output is mirrored to USB by the *secondary* console, so you still see logs -- input isn't). |
| `Component config -> Board configuration -> SMD board uses microSD instead of line 2` | **unchecked**, if Line 2's AG1171 is populated | If left checked while `CONFIG_BOARD_NUM_LINES=2`, line 1's SLIC pins resolve to `GPIO_NUM_NC` and `ag1170_init()` tries to configure invalid GPIOs. Symptom: `GPIO_PIN mask error` / `gpio_set_level(...): GPIO output gpio_num error` flooding the log right after the `board:` lines, sometimes severe enough to starve other tasks into a watchdog reset. |

**`-D "CONFIG_X=y"` on the command line is not reliable for these** — it only sets a
`build/CMakeCache.txt` entry, and a later `reconfigure`/`set-target`/plain `build` can
silently drop it back to the Kconfig default without any error, even if an earlier
build with the same flag succeeded. Confirm the setting actually stuck with
`grep CONFIG_WHATEVER sdkconfig` (project root, not `build/`) after building, and fix
it via `idf.py menuconfig` (writes directly to `sdkconfig`, survives `set-target`) if
it didn't. Press `/` in menuconfig to jump straight to a config symbol by name instead
of hunting through the menu tree.

At boot the firmware also runs a **runtime sanity check**: it probes the I2C
bus for both known codec addresses (ES8388 @ 0x10, WM8731 @ 0x1A) and logs a
warning if what it finds doesn't match the Kconfig board type you built for
(e.g. you flashed a THT build onto an SMD board). It can't switch the pinout
for you — the GPIOs really are wired differently per board — but it will
tell you loudly if you flashed the wrong one. Run it manually any time with
the `codec_detect` console command.

## 2. Install the host-side script dependencies

```bash
cd firmware/bringup
pip install -r requirements.txt
```

## 3. Find the serial port

```bash
python3 serial_probe.py
```

If it prints exactly one port, every script below will auto-detect it. If it
prints more than one (or none), pass `--port /dev/cu.usbserial-XXXX`
explicitly to each script.

## 4. Run the bring-up sequence

Either run everything in order:

```bash
python3 bringup_checklist.py
```

This runs each step below, in order, and stops at the first failure (each
step depends on the previous one working). Pass `--yes` to skip the
confirmation prompt between steps.

Or run one subsystem at a time while debugging a specific block:

| Script | What it checks |
|---|---|
| `test_mcu.py` | Board boots, console REPL responds, reports board type |
| `test_codec.py` | I2C scan finds the codec, `codec_init` succeeds, register dump looks sane |
| `test_audio.py` | Tone generation, ADC capture level, digital loopback (mic -> earpiece) |
| `test_slic.py` | SLIC power-up, hook-switch detection, ringing |
| `test_dtmf.py` | DTMF digit generation and call-progress tones (dial/ringback/busy) |

Each takes `--port`, `--baud`, and (except `test_mcu.py`) `--line 0|1`. Steps
that require a human to listen or lift a handset say so explicitly and print
`Manual check required` at the end — these can't be fully automated without
an audio analyzer on the line.

## Audio path on SMD2 (ES8388)

- **RX (mic/line-in):** LIN1 = Line 0, RIN1 = Line 1 — independent left/right
  ADC channels, both digitize simultaneously every I2S frame. (Originally
  Line 1's rx was wired into LIN2, sharing Line 0's left channel via a mux,
  with RIN1/RIN2 dead/grounded — bodge-fixed by moving Line 1's rx to RIN1;
  see `hardware/ROTARY_PHONE_SMD.md` and the comment in
  `firmware/components/es8388/include/es8388.h`.) `capture` and `loopback`
  see both lines' incoming audio at once.
- **TX (playback):** LOUT1 = Line 0, ROUT1 = Line 1, and both play back
  simultaneously (no mux limitation on this side) — `tone`/`dtmf_gen`/etc.
  work on both lines at once.
- **Debug header J5:** a 3-pin header (LOUT2, ROUT2, GND) for bench-testing
  with headphones. Per the ES8388 datasheet, LOUT2/ROUT2 always carry the
  same left/right mix as LOUT1/ROUT1 — it's a monitor tap of the same two
  lines' tx audio, not an independent signal.

## Manual console reference

If you'd rather drive the console by hand (`idf.py monitor` or any serial
terminal at 115200 8N1), the commands used by the scripts above are:

```
board_info
i2c_scan
codec_detect
codec_init
codec_dump
tone <hz> <ms> <line>
capture <ms>
loopback <line> <0|1>
dtmf_gen <digits> <line>
dialtone|ringback|busy <line>
silence <line>
slic <line> <pd|fr|rm> <0|1>
ring <line> <secs>
hookmon <secs>
status
```

`codec_bypass <0|1>` also exists but only does anything on the WM8731 (THT)
board — the ES8388 driver doesn't implement an analog bypass path yet; use
`loopback` (a DSP/I2S loopback) instead.

`echopulse <line> <on|off> [period_ms]` — periodic timestamped test tone
for characterizing hybrid/line echo on real hardware (oscilloscope or ear,
not a log-timestamp exercise). See [`../../archive/ECHO_DEBUGGING.md`](../../archive/ECHO_DEBUGGING.md).
