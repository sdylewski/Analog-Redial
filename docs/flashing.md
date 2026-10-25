[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · **3 · Flash & test** · [4 · Set up & use](setup.md)

# 3 · Flash & test

Put the firmware on an assembled board and check every part of the hardware
works. No ESP32 experience needed. Most of the time goes into installing the
toolchain, which you only do once.

## 1. What you need

- An assembled board ([step 2](building.md))
- A USB-C cable that carries data, not just power
- A computer running OSX, Linux or Windows
- An analog phone to test with

## 2. Install the toolchain (once)

The firmware is built with **ESP-IDF v5.2.8**, Espressif's official SDK. It
brings its own compiler and Python environment. On macOS or Linux:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.2.8 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
```

Then, **in every new terminal** you use for this project:

```bash
. ~/esp/esp-idf/export.sh
```

Windows, and more detail on every step:
[Espressif's getting-started guide](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/get-started/)
and the [firmware developer notes](https://github.com/sdylewski/Analog-Redial/blob/main/firmware/README.md).

## 3. Get the code

```bash
git clone https://github.com/sdylewski/Analog-Redial.git analog-redial
cd analog-redial/firmware
```

## 4. Flash it

Plug the board into your computer through the DevKit's USB-C port marked
**USB** (the right-hand one). The DevKitC-1 has two; the other, marked
**UART**, isn't connected to the console on this board. Then:

```bash
cd bringup
./flash_board.py --board smd4 --yes
```

Use `--board smd5` for a v5 board (including a 30-pin clone whose two solder jumpers were moved, see [the v5 reference](hardware-smd5.md#10-mcu-module-variants)); a clone with the jumpers left at the default needs `--board smd5_cheap`, which has no USB console. The script reads the chip's flash size,
builds the matching image and flashes it. Later flashes don't need
`--board`: the script asks the running firmware what it is.

<details>
<summary>Prefer to drive <code>idf.py</code> yourself?</summary>

Each board type builds in its own directory with its own settings. From
`firmware/`, for a v4 board (the default `build/` directory):

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

For a v5 board, create its directory once, then build and flash into it:

```bash
idf.py -B build.smd5 -D SDKCONFIG="build.smd5/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.smd5.defaults" set-target esp32c6
idf.py -B build.smd5 -D SDKCONFIG="build.smd5/sdkconfig" -p <PORT> flash monitor
```

Always pass the same `-B` and `-D SDKCONFIG=` pair, or the build can fall back to
the v4 settings.

`<PORT>` is the board's serial device: `/dev/cu.usbmodem…` on macOS,
`/dev/ttyACM…` on Linux, `COM…` on Windows. Leave `-p` off to let idf.py
find it. Quit the monitor with **Ctrl-]**.

On a module with **8 MB** of flash, use a separate directory with the 8 MB
settings added (`smd4` here; use `smd5` for a v5 board):

```bash
idf.py -B build.smd4_8mb -D SDKCONFIG="build.smd4_8mb/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.smd4.defaults;sdkconfig.8mb.defaults" \
  set-target esp32c6
idf.py -B build.smd4_8mb -D SDKCONFIG="build.smd4_8mb/sdkconfig" -p <PORT> flash
```

The number of phone lines and microSD aren't build options: set them after
flashing in Settings → General → Board hardware (or with
`first_setup.py --lines`).

</details>

## 5. Watch it boot

Open the serial monitor (`idf.py monitor` from `firmware/`). You'll see each
part come up (codec, phone lines, audio, telephony) ending with:

```
All subsystems up. Lift a handset to get dial tone.
```

Lift the handset: you should hear dial tone.

> **Opening the serial port resets the board.** That's how the ESP32-C6's
> USB works, not a fault, so connecting a monitor mid-call ends the call.

## 6. Test the hardware
Note: a lot of these functions are available in the web UI, but the console gives you more control and debugging capability.

The same USB connection gives you a console that works with or without
Wi-Fi. Type `help` for the full list. A good first run on a new board:

1. `selftest`: a guided check of the board's hardware
2. `hookmon 20`: lift and replace the handset a few times and watch it register
3. `dialtone 0`: dial tone on line 0
4. `ring 0 3`: rings the bell for 3 seconds

| Command | What it does |
|---------|--------------|
| `selftest` | Guided hardware check; start here |
| `board_info` | Board type and pin map the firmware is using |
| `codec_detect` | Checks the audio codec answers |
| `status` | State of each phone line |
| `hookmon [secs]` | Shows hook-switch changes as you lift and replace the handset |
| `pulsemon [secs]` | Shows rotary dial pulses with timing |
| `tonemon [secs]` | Shows touch-tone digits |
| `dialtone [line]` / `silence [line]` | Dial tone on or off |
| `ring [line] [secs]` | Ring the bell |
| `capture [ms]` | Live input level meter for both lines |
| `miclevel [secs]` | Handset microphone level; talk into it |
| `spkamp` / `micamp` | Earpiece volume and microphone gain, live |
| `sd_test` | microSD card info plus a write/read check |
| `wifi_status` / `wifi_set <ssid> <pass>` | Check or set Wi-Fi without the web interface. For a network name with an apostrophe, accent or emoji (an iPhone hotspot's "Name’s iPhone"), run `wifi_scan` and use `wifi_set #3 <pass>`; `wifi_set -u` takes `%XX` escapes for any bytes |
| `reboot` | Restart, keeping the monitor attached |

## 7. Preparing boards for someone else

To set up a batch of boards that each come up fresh for whoever gets them:

1. Flash each board and set it up on your own Wi-Fi.
2. Run its checks: `./first_setup.py --host <board> --tests`, or the
   bring-up check under Settings → Diagnostics.
3. Finish with `./first_setup.py --host <board> --reset-wifi`. This forgets
   your Wi-Fi and the board name, keeps everything else, and reboots into
   setup mode.
4. Unplug it once its setup network appears.

The recipient powers it up and follows [step 4](setup.md). Full script
reference: [`firmware/bringup/README.md`](https://github.com/sdylewski/Analog-Redial/blob/main/firmware/bringup/README.md).

---

**Next: [4 · Set up & use](setup.md)**
