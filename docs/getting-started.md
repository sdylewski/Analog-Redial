# Getting Started

A from-scratch guide to building, flashing, and setting up **Analog Redial** —
turning an analog (rotary or touch-tone) telephone into a networked phone
that plays audio clips, records messages, calls a second handset completely
offline, streams internet radio, and places VoIP calls over Wi-Fi.

New here? This is the doc to start with. It assumes no prior ESP32
experience. For deeper detail see [`../README.md`](../README.md),
[`features.md`](features.md), [`../firmware/README.md`](../firmware/README.md),
and [`hardware-smd4.md`](hardware-smd4.md).

---

> **Need to assemble the board first?** See
> [`building.md`](building.md) — hand-solder order, pre-power checks and
> first power-on. Come back here once it powers up cleanly.

## 1. What you need

**Hardware** — an SMD4 or SMD5 board ([`hardware-smd4.md`](hardware-smd4.md)):

- ESP32-C6-DevKit
- ES8388 stereo codec
- 1–2× Silvertel AG1171 SLIC (one per phone line)
- 1 or 2 analog (rotary or touch-tone) telephones on tip/ring
- **A 2 A supply** — USB-C into the DevKit, or a 5.5–6 V brick on the DC
  jack. See [`bom.md`](bom.md) for why 2 A rather than 1 A.
- A 2.4 GHz Wi-Fi network

(Hardware design files are under [`../hardware/`](../hardware/); a BOM/shopping
checklist is at [`bom.md`](bom.md).)

**Software** — **ESP-IDF v5.2.8** and its toolchain (this is Espressif's
official SDK; it brings its own compiler and Python environment).

---

## 2. Install the toolchain (ESP-IDF)

Full official instructions:
<https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/get-started/>.
The short version (macOS/Linux):

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.2.8 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
```

Then, **in every new terminal** you use for this project:

```bash
. ~/esp/esp-idf/export.sh      # puts idf.py + the toolchain on your PATH
```

See [`../firmware/README.md`](../firmware/README.md) for a more detailed
toolchain walkthrough (including Windows).

---

## 3. Get the code

```bash
git clone https://github.com/sdylewski/Analog-Redial.git analog-redial
cd analog-redial/firmware
```

All firmware lives under `firmware/`. You run `idf.py` from there.

---

## 4. Configure, build and flash

**Easiest path — `flash_board.py`.** It figures out the board type (from
whatever's already flashed, or asks you once) and the flash size (reads the
chip directly), picks or creates the matching build directory, and flashes:

```bash
cd firmware/bringup
. ~/esp/esp-idf/export.sh
./flash_board.py --board smd4 --yes      # first flash on a given board: say which board
./flash_board.py --yes                   # later flashes: detects it from the running firmware
```

Add `--setup --host <name-or-ip>` to run `first_setup.py` right after (sets
line count, uploads the default prompts) — see
[`../firmware/bringup/README.md`](../firmware/bringup/README.md) for the
full reference on both scripts, including the 8 MB flash layout and the
hardware bring-up checklist.

**Manual path, if you'd rather drive `idf.py` yourself:**

```bash
idf.py menuconfig
```

Under **Board configuration**:

| Option | Choose |
|--------|--------|
| **Target board type** | **Set this first.** `SMD3/SMD4` for an SMD4 board; `SMD5` for an SMD5 with the genuine 32-pin module; `SMD5 (clone module)` for an SMD5 with the 30-pin AliExpress module. Getting this wrong on a clone-populated board costs you audio entirely — MCLK lands on the wrong pin. |
| **Number of active phone lines** | `1` or `2` (single-line boards leave line 2 unpopulated) |
| **Use microSD instead of line 2** | Enable **only** if your board has the microSD socket stuffed instead of the line-2 SLIC (they share GPIO 18–21). Requires *lines = 1*. |

> The Kconfig **default is the old through-hole board**, not SMD4 — the
> tracked `sdkconfig` already selects SMD4, but if you ever regenerate the
> config from scratch, set the board type before anything else.

Save and exit — the line count and SD-vs-line-2 routing follow from these
choices. (Wi-Fi and VoIP are **not** set here; those are configured at
runtime — see below.)

> **Note on committing config:** `sdkconfig` holds your board's choices. The
> repo's committed default is the **2-line, no-SD** build. Your local
> `menuconfig` edits change `sdkconfig` for your board; that's expected.

Then, with the board plugged in over USB:

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

`<PORT>` is your board's serial device — e.g. `/dev/cu.usbmodemXXXX` (macOS),
`/dev/ttyUSBX` or `/dev/ttyACMX` (Linux), `COMx` (Windows). If `-p` is omitted,
idf.py tries to auto-detect. `monitor` shows the live log; quit with
**Ctrl-]**.

---

## 5. Watch it boot

You should see the subsystems come up (`board`, `audio`, codec, SLIC,
`telephony`, …) ending in **"All subsystems up. Lift a handset to get dial
tone."** A serial **console** is also running — type `help` for bring-up/test
commands (`sd_test`, `sip_status`, `record`, `capture`, …).

---

### The bring-up console

The same USB connection you flashed over gives you an interactive console —
`idf.py monitor`, then type `help`. It works whether or not Wi-Fi is up,
which makes it the tool of choice when something isn't behaving.

The ones worth knowing on a new board:

| Command | What it does |
|---------|--------------|
| `selftest` | Guided hardware bring-up check — start here on a fresh board |
| `board_info` | Detected board type and pin map |
| `codec_detect` | Probes the audio codec, and warns if it doesn't match the firmware's board type |
| `status` | Per-line telephony state |
| `hookmon [secs]` | Watch the hook switch — lift and drop the handset and see it register |
| `pulsemon [secs]` | Watch rotary pulses with timing |
| `tonemon [secs]` | Watch touch-tone digits |
| `dialtone [line]` / `silence [line]` | Force dial tone on or off, bypassing the dial plan |
| `ring [line] [secs]` | Ring the bell |
| `capture [ms]` | Live RX level meter for both lines |
| `miclevel [secs]` | Handset mic level and clipping check — talk into the handset |
| `spkamp` / `micamp` | Earpiece drive and mic gain, live |
| `wifi_status` / `wifi_set <ssid> <pass>` | Check or set Wi-Fi without the web UI |
| `wifi_list` / `wifi_forget <ssid>` | List or remove saved networks |
| `power` | Low-power idle state |
| `reboot` | Software restart that keeps the monitor attached |
| `download` | Reboot into the ROM bootloader for flashing |

A good first sequence on a new board: `selftest`, then `hookmon 20` while
you lift and replace the handset, then `dialtone 0` and listen.

> **Opening the serial port resets the ESP32-C6.** That's the USB-JTAG
> hardware, not a bug — so a monitor session that starts mid-call will end
> that call, and reconnecting drops any Bluetooth link.

---

## 6. First-boot Wi-Fi setup (no credentials yet)

On first boot there are no Wi-Fi credentials, so the phone brings up **its own
Wi-Fi network** for setup:

1. On your phone/laptop, join the open network **`AnalogRedial-XXXX`** (XXXX =
   last MAC digits, shown in the log).
2. A **setup page** should pop up automatically (captive portal). If not,
   browse to **`http://192.168.4.1`**.
3. Enter your home Wi-Fi name + password and submit. The page shows a large
   address to go to once it reboots — see below for what that address is
   and why it's not always the same one.

Alternatively, from the serial console: `wifi_set "<ssid>" "<password>"`.

### Its address: `phone.local`

The setup page has an optional **Board name** field. Leave it blank and the
board is reachable at **`phone.local`**. The confirmation page shows the
address in large text, so you know where to go once it reboots.

**More than one board?** Before claiming a name, the board checks whether
anything on the network already answers to it. If so, it uses `phone1` (then
`phone2`, and so on) — the web UI, its `.local` address and your router's
device list all show the same name. That numbered name lasts only until the
next reboot, so a board moved to a network where it's alone goes back to
`phone`. To keep a name, set one in Settings → General.

### Preparing boards to give away

To set up and check a stack of boards that each come up as `phone.local` for
whoever receives them:

1. Flash and set up each board on your own network, and run its checks
   (`first_setup.py --tests`, or the Diagnostics page's bring-up check).
2. Press **Settings → Hardware → Reset → Reset WiFi** (or add `--reset-wifi`
   to the `first_setup.py` command). It forgets the Wi-Fi networks and the
   board name, keeps everything else, and reboots into setup mode.
3. Unplug it once its setup network appears.

The recipient powers it up, joins the setup network, enters their Wi-Fi, and
opens `phone.local`. Reset WiFi is also what to use when you move a board to
a different network yourself.

**`.local` addresses aren't reliable on every network.** If it doesn't load
within a minute, open your Wi-Fi router's admin page and look at its list of
connected devices instead — the board shows up there under the same name,
and opening its IP address directly works every time, `.local` or not.

> **Multiple phones on one network?** Give each a unique **Board name** under
> Settings → General — it becomes the new `<name>.local` address and labels
> the UI, replacing the default `phone`.

---

## 7. Status LED reference

One onboard RGB LED (the ESP32-C6-DevKitC-1 module's own, GPIO8) reports
what the board is doing, so you don't need a monitor session open just to
tell "still booting" from "stuck." Deliberately dim — it's a status light,
not a room light.

| Color + pattern | Meaning |
|---|---|
| **Red, fast blink** (~100 ms) | Booting — subsystems still coming up |
| **Red, slow blink** (~500 ms) | A subsystem failed to init, but the board kept running anyway |
| **Green, slow blink** (~500 ms) | Trying to join Wi-Fi, **or** sitting in setup mode with no network to join — these look identical. If it's been blinking green for more than a few seconds right after boot, it's probably setup mode: see [§6](#6-first-boot-wi-fi-setup-no-credentials-yet) |
| **Green, solid** (30 s, then off) | Just connected to Wi-Fi successfully |
| **Off** | Idle and working normally — Wi-Fi connected, nothing else going on |
| **Yellow (red+green), fast blink** (~300 ms) | Writing a new firmware image (web UI firmware upload) — don't power off |
| **Blue, slow blink** (~500 ms) | Bluetooth companion is discoverable, waiting to pair (SMD5 only) |
| **Blue, solid** (5 s, then reverts) | Bluetooth just paired/connected |

**SMD2 has a single-color LED** (plain red, not RGB) on a different GPIO —
same blink timing for booting/error/Wi-Fi, but it has no separate pattern
for firmware updates or Bluetooth, so those briefly look identical to
"booting" there.

**The chip's own ROM bootloader runs before any of this firmware exists to
light anything.** If the board is stuck in USB download mode (e.g. after a
failed flash), the LED won't tell you — a boot log over `idf.py monitor` or
`esptool ... flash_id` is the way to check that state, not the LED.

---

## 8. The web UI

- **Status** — live line state, Wi-Fi, uptime, heap, storage, a recent-
  activity panel (calls, recordings, dial errors), and the dial guide: the
  live dial plan (generated from firmware, always accurate). Cards that
  don't apply to your build (e.g. "Call other phone" on a 1-line board) are
  hidden. (`/guide` still works as a link — it just redirects here.)
- **Recordings** — messages left with dial code 211 (duration, timestamp,
  download, delete).
- **Settings** — six tabs: **General** (board name, Wi-Fi, language,
  digit readback), **Dial Codes & Prompts** (the editable clip table, the
  Operator greeting, and the Record-message prompt/length — all "what plays
  for a code" in one place; recode or remove an existing entry, add a new
  one by uploading its audio directly), **VOIP**, **Hardware** (chip/codec/
  serial info, the SMD line-2/microSD config, and per-line features like the
  answering machine), **Storage** (the full SD/internal file manager — kept
  separate from Hardware since listing files is the slowest thing on this
  page and shouldn't cost anything on tabs that don't need it), and
  **Diagnostics** (log level, the full log, test-mode controls).

---

## 9. Optional setup

**Audio clips & prompts.** Two ways to get WAVs onto the phone:

- **Bundled at build time** — a short, explicit list in `firmware/main/
  CMakeLists.txt` (`BUNDLED_AUDIO`) gets baked into a dedicated `bundled`
  flash partition and reflashed with every `idf.py flash`, without ever
  touching recordings in the writable `storage` partition. Three files ship
  by default: `operator.wav`, the answering machine's default greeting
  (`am_greeting.wav`), and a public-domain dial-up modem sound
  (`dialup.mp3`). Anything else you drop in `firmware/assets/` stays on your
  disk and is **not** built in unless you add it to that list — add your own
  clips, from audio you own the rights to:

  ```bash
  cd firmware/bringup
  ./sync_pi_audio.py --src /path/to/your/audio --out-dir ../assets   # batch
  ./gen_prompt.py --from song.mp3 --name clip_401.wav --out-dir ../assets  # one file
  ```

  Files added this way have to either be uploaded to a board at runtime (see
  below) or added to `BUNDLED_AUDIO` to ship with every build. See
  [`../firmware/assets/README.md`](../firmware/assets/README.md) for the
  full mechanism and override rules.

- **Uploaded at runtime** — `gen_prompt.py --upload phone.local` (or the web
  UI's Settings → Storage file manager) writes straight into the writable
  partition, no rebuild/reflash needed. A same-named upload always overrides
  a bundled default.

**Browser upload.** Settings → Operator and Settings → Dial Codes both have a
real file-upload form — pick a WAV (any common sample rate, 8/16/24/32-bit,
mono or multi-channel) and it's converted to 8kHz mono automatically before
being stored, no host-side conversion step needed. MP3 upload is available
too if you've enabled `CONFIG_AUDIO_CONVERT_MP3` in menuconfig (off by
default — real RAM cost, not just flash; see that option's help text).

**Dial Codes.** Settings → Dial Codes covers all the "silly clip" style
codes: each one's dial code AND its audio file are independently editable at
runtime, no rebuild — recode an existing one, upload new audio for it, or add
a brand new code entirely. Stored in flash (NVS), takes effect immediately.
Operator help (dial `0`) is separate — its code is fixed; replace its audio
from Settings → Operator.

**VoIP (SIP).** Settings → VOIP has a form (server, per-line
extension/username/password) plus a "Download Asterisk server setup script"
link that generates a ready-to-run script for your Asterisk/PBX box. Save the
form, reboot, then run the downloaded script on the Asterisk machine (not the
phone) and check the registration status back on that same page. Dial `555`
+ a 4-digit extension to place a call. LAN only (no TLS/SRTP). The console's
`sip_set`/`sip_status` commands still work too, if you prefer that. See
[`voip-setup.md`](voip-setup.md) for the full walkthrough.

**microSD.** If you built with microSD enabled, the boot log shows whether
the card mounted; `sd_test` from the console prints card info and runs a
write/read/verify.

---

## 10. The BOOT button: rescue and factory reset

Two gestures, both with the board **powered on and running** — hold the
**BOOT button** on the DevKit (GPIO9):

| Hold | What happens | Credentials |
|------|--------------|-------------|
| **~3 s, then release** | Reboots into the setup Wi-Fi network so you can reach the full web UI from a phone | **Kept** |
| **~10 s** | Erases saved Wi-Fi credentials and reboots into setup (section 6) | **Erased** |

The LED changes at the 3-second mark, so you know when to let go.

**The 3-second gesture is the one you want on site.** If the board can't
join the network it's in front of — a demo at someone else's office, a
mistyped password, a router that's changed — hold BOOT for three seconds and
release. It comes back advertising its own open network, named after the
board (`Analog Redial - telephono` for a board called *telephono*, or
`AnalogRedial-XXXX` if it hasn't been named yet);
join that from a phone or laptop and browse to **192.168.4.1** for the normal
dashboard, settings, dial codes, Bluetooth pairing, everything. A banner
reminds you you're in rescue mode. It's one-shot — the next ordinary reboot
rejoins your saved network.

> Do this at **runtime, not at power-on** — GPIO9 held low *during reset*
> just enters the chip's USB flashing mode.

---

## See also

| Doc | For |
|-----|-----|
| [`../firmware/README.md`](../firmware/README.md) | Toolchain detail, console command reference, build layout |
| [`../firmware/bringup/README.md`](../firmware/bringup/README.md) | `flash_board.py`/`first_setup.py` reference, 8 MB flash layout, hardware checklist |
| [`features.md`](features.md) | What's implemented + firmware architecture |
| [`hardware-smd4.md`](hardware-smd4.md) | SMD4 schematic, layout, power tree |
| [`building.md`](building.md) | Assembling a board before you flash it |
| [`hardware-smd5.md`](hardware-smd5.md) | Next-spin board (Bluetooth, LDO, dual MCU footprint) — not yet fabricated |
| [`bom.md`](bom.md) | Parts list / shopping checklist |
| [`voip-setup.md`](voip-setup.md) | Connecting to a SIP server, dial-plan reference |
| [`../archive/signal-levels.md`](../archive/signal-levels.md) | Audio/logic levels reference |

Questions or a board that behaves oddly? The serial `help` console is the
fastest way to poke at hardware (hook, ring, DTMF, audio, SD, SIP) directly.
