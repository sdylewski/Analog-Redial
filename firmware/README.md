# Rotary Phone Gateway — ESP32-C6 firmware

ESP-IDF firmware for the two-line rotary phone SIP gateway. Replaces the
Raspberry Pi software (`../../pi_rotary_phone`) with on-chip peripherals:
WM8731 codec (I²S/I²C), two AG1170 SLICs (GPIO), and an embedded SIP/RTP path
(VoIP, final milestone).

See [`../FEATURES.md`](../FEATURES.md) for the feature port matrix and
architecture, and [`../hardware/WM8731.md`](../hardware/WM8731.md) for the GPIO
map (the firmware mirrors it in `components/board/include/board_pins.h`).

**SMD board (`board_pins_smd.h`):** GPIO **18–21** = Line 2 SLIC **or** microSD (XOR). Match `CONFIG_BOARD_*` to BOM — see [`ESP32_PORT_PLAN.md`](../ESP32_PORT_PLAN.md) and [`hardware/BOM-smd.md`](../hardware/BOM-smd.md).

## Requirements

- **ESP-IDF v5.2 or newer** (uses the `i2c_master` and `i2s_std` drivers).
- Target: **esp32c6** (ESP32-C6-DevKitM-1 or your gateway PCB).

## Toolchain setup (macOS)

These steps install ESP-IDF and the cross-compiler/tools on your Mac. You only
run the install once; after that you just `get_idf` (or `source export.sh`) in
each new terminal before building.

Official reference:
[ESP-IDF macOS setup (ESP32-C6)](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/get-started/linux-macos-setup.html).

### 1. Prerequisites

Install Xcode command-line tools (compiler, `git`, etc.):

```bash
xcode-select --install
```

Install build helpers with Homebrew (you already have `cmake` from Homebrew; add
the rest):

```bash
brew install cmake ninja dfu-util ccache python3
```

Check Python 3 is available (`python3 --version` should be 3.8+).

### 2. Clone ESP-IDF

Pick a fixed install location (default `~/esp`). Use a **v5.2+** release branch
— this project was written against the v5.2 driver APIs:

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.2 --recursive https://github.com/espressif/esp-idf.git
```

That creates `~/esp/esp-idf`. To use a newer stable release later (e.g. v5.3),
swap the branch tag; then re-run `./install.sh esp32c6` after `git pull`.

### 3. Install tools (compiler, OpenOCD, Python venv, …)

Install the toolchain **for ESP32-C6 only** (faster than `all`):

```bash
cd ~/esp/esp-idf
./install.sh esp32c6
```

This downloads compilers and utilities into `~/.espressif/` and may take several
minutes on first run.

If GitHub downloads are slow or fail, use Espressif’s mirror before `install.sh`:

```bash
export IDF_GITHUB_ASSETS="dl.espressif.com/github_assets"
./install.sh esp32c6
```

On Apple Silicon, if you see `bad CPU type in executable` for an old tool, install
Rosetta once: `/usr/sbin/softwareupdate --install-rosetta --agree-to-license`

If Python SSL errors appear on macOS, run Apple’s certificate installer for your
Python (often `Applications/Python 3.x/Install Certificates.command`) — see
[ESP-IDF tool install troubleshooting](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/get-started/linux-macos-setup.html).

### 4. Activate the environment (every new shell)

ESP-IDF uses a Python virtualenv and PATH changes that are **not** global. In
each terminal where you want `idf.py`:

```bash
. $HOME/esp/esp-idf/export.sh
```

Add a shortcut to `~/.zshrc` so you can type `get_idf` instead:

```bash
echo 'alias get_idf=". $HOME/esp/esp-idf/export.sh"' >> ~/.zshrc
source ~/.zshrc
```

Then in any new terminal:

```bash
get_idf
idf.py --version    # should print ESP-IDF v5.2.x
```

Do **not** put `export.sh` in `.zshrc` permanently — it activates the IDF
virtualenv in every shell and can interfere with other Python tools.

### 5. Find the serial port

Plug in the ESP32-C6 DevKitM-1 (USB). On macOS the port is usually under
`/dev/cu.*` (use `cu`, not `tty`, for flashing):

```bash
ls /dev/cu.usb*
```

Typical names: `/dev/cu.usbmodem101` or `/dev/cu.usbmodem1101`. Unplug/replug
and run `ls` again if unsure which device appeared.

The DevKit’s built-in **USB Serial/JTAG** is the default console for this
firmware (`phone>` prompt). The PCB’s UART headers (J4/J5, GPIO16/17) are an
alternate if you wire them.

### 6. Verify with a smoke test (optional)

After `get_idf`, confirm the toolchain can see your chip:

```bash
cd ~/esp/esp-idf/examples/get-started/hello_world
idf.py set-target esp32c6
idf.py -p /dev/cu.usbmodemXXXX build flash monitor
```

Exit the monitor with **Ctrl+]**. Then build this project (below).

### Cursor / VS Code (optional)

The [Espressif IDF extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
can run `idf.py build flash monitor` from the UI. Point it at `~/esp/esp-idf`
and set the project directory to this repo’s `firmware/` folder.

## Build / flash / monitor

```bash
get_idf    # or: . $HOME/esp/esp-idf/export.sh

cd /path/to/esp32_rotary_phone/firmware
idf.py set-target esp32c6    # first time only (or after changing target)
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Replace `/dev/cu.usbmodemXXXX` with your port from step 5 above.

### Monitor keys

`idf_monitor` intentionally does **not** bind Ctrl+C to quit — Ctrl+C is
passed through to the target instead. Use:

| Keys | Action |
|------|--------|
| **Ctrl+]** | Quit |
| **Ctrl+T, Ctrl+X** | Quit (alternate, if your terminal app eats Ctrl+]) |
| **Ctrl+T, Ctrl+H** | Full shortcut list (menu help) |
| **Ctrl+T, Ctrl+R** | Reset the target (RTS pin toggle) without leaving the monitor |

`Ctrl+T` is the menu prefix — press it, release, then press the second key.

> No PCB yet? You can still flash a bare DevKitM-1 and run the console — the
> hardware-dependent commands fail gracefully (e.g. `i2c_scan` finds nothing,
> `hookmon` reads floating pins). The DSP commands (tones, DTMF gen) and the
> state machine run regardless.

## Bring-up checklist (serial console)

After boot you get a `phone>` prompt. The full telephony stack is already
running concurrently. Type `help` for the full list. Recommended order:

| Step | Command | Pass criteria |
|------|---------|---------------|
| 1. Power rails | (multimeter) | 3.3 V, 1.8 V (DCVDD via U3), 5 V to SLICs |
| 2. I²C bus | `i2c_scan` | Reports `0x1A  <- WM8731` |
| 3. Codec config | `wm8731_init` then `wm8731_dump` | `init: ESP_OK`; sane registers |
| 4. Tone out (DAC) | `tone 1000 1500 0` | 1 kHz heard on Line 0 handset; `tone 1000 1500 1` for Line 1 |
| 5. Call-progress | `dialtone 0`, `ringback 0`, `busy 0`, `silence 0` | Correct cadences |
| 6. Capture (ADC) | `capture 4000` (talk/whistle into handset mic) | `rms` rises with sound |
| 7. Loopback | `loopback 0 1` (talk) → `loopback 0 0` | Hear your own voice (codec + I²S round trip) |
| 8. Codec bypass | `wm8731_bypass 1` | Analog line-in straight to out (isolates DSP from analog) |
| 9. SLIC power | `slic 0 pd 1` (run), `status` | Line powered |
| 10. Ring | `ring 0 6` | Bell rings ~2 s on / 4 s off |
| 11. Hook/pulse | `hookmon 15` (lift handset, dial digits) | SHK toggles; pulse breaks visible |
| 12. DTMF gen | `dtmf_gen 0123456789 0` | Tones audible / detectable |
| 13. DTMF detect | inject DTMF on the line; watch logs / `status` | Digits decoded |
| 14. Dialing end-to-end | lift handset → dial `369`/`842`/codes | Dial tone, digit collection, action logs |

### Console command reference

| Command | Description |
|---------|-------------|
| `board_info` | Board type, line count, I2C pins |
| `i2c_scan` | Probe the I²C bus for the codec |
| `codec_detect` | Probe both known codec addresses; warn on board-type mismatch |
| `codec_init` | Initialize the codec (ES8388 on SMD, WM8731 on THT) |
| `codec_dump` | Print shadowed codec registers |
| `codec_bypass <0\|1>` | Analog line-in → line-out bypass (WM8731 only) |
| `tone <hz> <ms> <line>` | Arbitrary test tone on a line |
| `dialtone\|ringback\|busy [line]` | Call-progress tones |
| `silence [line]` | Stop audio on a line |
| `loopback [line] [0\|1]` | Mic → own speaker round-trip |
| `capture [ms]` | RX RMS level meter for both lines |
| `dtmf_gen <digits> [line]` | Emit DTMF digits |
| `slic <line> <pd\|fr\|rm> <0\|1>` | Raw SLIC pin control |
| `ring [line] [secs]` | Ring cadence on a line |
| `hookmon [secs]` | Live SHK / hook state |
| `status` | Per-line telephony state |

See [`firmware/bringup/README.md`](bringup/README.md) for host-side scripts
that drive this console over serial, one per subsystem (MCU, codec, audio,
SLIC, DTMF), plus a `bringup_checklist.py` that runs the full sequence.

## Layout

```
firmware/
├── CMakeLists.txt          # project
├── sdkconfig.defaults      # target esp32c6, console, 4MB flash, custom parts
├── partitions.csv          # factory app + SPIFFS asset storage
├── main/app_main.c         # boot + wire-up + console
└── components/
    ├── board/      # GPIO map (board_pins.h) + I2C bus init + codec auto-detect
    ├── wm8731/     # codec I2C driver (THT board)
    ├── es8388/     # codec I2C driver (SMD boards)
    ├── audio/      # I2S engine, routing, RX taps (VoIP seam)
    ├── tones/      # dual-tone + call-progress synth
    ├── dtmf/       # DTMF gen + Goertzel detect
    ├── ag1170/     # SLIC: hook debounce, pulse dial, ring cadence
    ├── dialplan/   # dial-code table (ported from DIAL_CODES.md)
    ├── telephony/  # per-line call state machine
    ├── voip/       # SIP/RTP interface (stub now; impl last)
    └── bringup/    # esp_console test harness
```

## Notes / current limitations

- **VoIP is a stub** (`components/voip`). The interface is finalized so the
  state machine and audio taps are VoIP-ready; the SIP/RTP implementation is
  the last milestone. Dialing `555-xxxx` currently returns reorder tone.
- **Audio assets** (clips, prompts, recordings) are not yet stored in flash —
  those dial codes log their action and play a confirmation tone. The SPIFFS
  `storage` partition is reserved for them.
- Audio runs at **48 kHz stereo** so the codec sees a clean 12.288 MHz MCLK
  (256fs). VoIP will resample to 8 kHz G.711 in the `voip` component.
- `PD` drive polarity is set in `board_pins.h` (`BOARD_PD_LEVEL_RUN`). Verify
  against the actual transistor stage on your PCB before ringing.
