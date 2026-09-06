# Getting Started

A from-scratch guide to building, flashing, and setting up **Analog Redial** —
turning an analog (rotary or touch-tone) telephone into a networked phone
that plays audio clips, records messages, calls a second handset completely
offline, streams internet radio, and places VoIP calls over Wi-Fi.

New here? This is the doc to start with. It assumes no prior ESP32
experience. For deeper detail see [`../README.md`](../README.md),
[`features.md`](features.md), [`../firmware/README.md`](../firmware/README.md),
and [`hardware.md`](hardware.md).

---

## 1. What you need

**Hardware** — the SMD4 board ([`hardware.md`](hardware.md)):

- ESP32-C6-DevKit
- ES8388 stereo codec
- 1–2× Silvertel AG1171 SLIC (one per phone line)
- 1 or 2 analog (rotary or touch-tone) telephones on tip/ring
- A USB-C cable or 5 V DC supply, and a 2.4 GHz Wi-Fi network

(Hardware design files are under [`../hardware/`](../hardware/); a BOM/shopping
checklist is at [`bom.md`](bom.md).)

**Software** — **ESP-IDF v5.2** and its toolchain (this is Espressif's
official SDK; it brings its own compiler and Python environment).

---

## 2. Install the toolchain (ESP-IDF)

Full official instructions:
<https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/get-started/>.
The short version (macOS/Linux):

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.2 --recursive https://github.com/espressif/esp-idf.git
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
git clone <this-repo-url> analog-redial
cd analog-redial/firmware
```

All firmware lives under `firmware/`. You run `idf.py` from there.

---

## 4. Configure the build (`idf.py menuconfig`)

```bash
idf.py menuconfig
```

Under **Board configuration**:

| Option | Choose |
|--------|--------|
| **Number of active phone lines** | `1` or `2` (single-line boards leave line 2 unpopulated) |
| **Use microSD instead of line 2** | Enable **only** if your board has the microSD socket stuffed instead of the line-2 SLIC (they share GPIO 18–21). Requires *lines = 1*. |

Save and exit — the line count and SD-vs-line-2 routing follow from these
choices. (Wi-Fi and VoIP are **not** set here; those are configured at
runtime — see below.)

> **Note on committing config:** `sdkconfig` holds your board's choices. The
> repo's committed default is the **2-line, no-SD** build. Your local
> `menuconfig` edits change `sdkconfig` for your board; that's expected.

---

## 5. Build, flash, and watch it boot

Plug the board in over USB, then:

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

`<PORT>` is your board's serial device — e.g. `/dev/cu.usbmodemXXXX` (macOS),
`/dev/ttyUSBX` or `/dev/ttyACMX` (Linux), `COMx` (Windows). If `-p` is omitted,
idf.py tries to auto-detect. `monitor` shows the live log; quit with
**Ctrl-]**.

You should see the subsystems come up (`board`, `audio`, codec, SLIC,
`telephony`, …) ending in **"All subsystems up. Lift a handset to get dial
tone."** A serial **console** is also running — type `help` for bring-up/test
commands (`sd_test`, `sip_status`, `record`, `capture`, …).

---

## 6. First-boot Wi-Fi setup (no credentials yet)

On first boot there are no Wi-Fi credentials, so the phone brings up **its own
Wi-Fi network** for setup:

1. On your phone/laptop, join the open network **`RotaryPhone-XXXX`** (XXXX =
   last MAC digits, shown in the log).
2. A **setup page** should pop up automatically (captive portal). If not,
   browse to **`http://192.168.4.1`**.
3. Enter your home Wi-Fi name + password and submit. The device saves them and
   reboots onto your network.

Alternatively, from the serial console: `wifi_set "<ssid>" "<password>"`.

Once connected, the log prints the device's IP and its mDNS name (default
**`phone.local`**). Open **`http://phone.local/`** (or the IP) in a browser.

> **Multiple phones on one network?** Give each a unique **Board name** under
> Settings → General; it becomes the `<name>.local` address and labels the UI.

---

## 7. The web UI

- **Status** — live line state, Wi-Fi, uptime, heap, storage, a recent-
  activity panel (calls, recordings, dial errors), and the dial guide: the
  live dial plan (generated from firmware, always accurate). Cards that
  don't apply to your build (e.g. "Call other phone" on a 1-line board) are
  hidden. (`/guide` still works as a link — it just redirects here.)
- **Recordings** — messages left with dial code 211 (duration, timestamp,
  download, delete).
- **Settings** — five tabs: **General** (board name, Wi-Fi, language,
  digit readback), **Dial Codes & Prompts** (the editable clip table, the
  Operator greeting, and the Record-message prompt/length — all "what plays
  for a code" in one place), **VOIP**, **Hardware** (chip/codec/serial info,
  the SMD line-2/microSD config, and the full SD+internal file manager), and
  **Diagnostics** (log level, the full log, test-mode controls).

---

## 8. Optional setup

**Audio clips & prompts.** Two ways to get WAVs onto the phone:

- **Bundled at build time** — anything you drop in `firmware/assets/` gets
  baked into a dedicated `bundled` flash partition and reflashed with every
  `idf.py flash`, without ever touching recordings in the writable `storage`
  partition. Only `operator.wav` ships by default (original project text, not
  copyrighted); everything else is gitignored on purpose — add your own, from
  audio you own the rights to:

  ```bash
  cd firmware/bringup
  ./sync_pi_audio.py --src /path/to/your/audio --out-dir ../assets   # batch
  ./gen_prompt.py --from song.mp3 --name clip_401.wav --out-dir ../assets  # one file
  ```

  See [`../firmware/assets/README.md`](../firmware/assets/README.md) for the
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

## 9. Resetting Wi-Fi (factory reset)

With the board **powered on and running**, short **GPIO9 (the BOOT strap)**
to GND (or hold the BOOT button) for **~3 seconds**. It erases the saved
Wi-Fi credentials and reboots into the setup AP (section 6).

> Do this at **runtime, not at power-on** — GPIO9 held low *during reset*
> just enters the chip's USB flashing mode.

---

## See also

| Doc | For |
|-----|-----|
| [`../firmware/README.md`](../firmware/README.md) | Toolchain detail, console command reference, build layout |
| [`features.md`](features.md) | What's implemented + firmware architecture |
| [`hardware.md`](hardware.md) | SMD4 schematic, pin map, power tree |
| [`bom.md`](bom.md) | Parts list / shopping checklist |
| [`voip-setup.md`](voip-setup.md) | Connecting to a SIP server, dial-plan reference |
| [`signal-levels.md`](signal-levels.md) | Audio/logic levels reference |

Questions or a board that behaves oddly? The serial `help` console is the
fastest way to poke at hardware (hook, ring, DTMF, audio, SD, SIP) directly.
