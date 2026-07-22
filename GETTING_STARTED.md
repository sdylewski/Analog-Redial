# Getting Started

A from-scratch guide to building, flashing, and setting up the ESP32-C6 Rotary
Phone Gateway — turning a vintage rotary (or touch-tone) telephone into a
networked phone that plays audio clips, records messages, calls a second
handset, streams internet radio, and places VoIP calls over Wi-Fi.

New here? This is the doc to start with. It assumes no prior ESP32 experience.
For deeper detail see [`README.md`](README.md), [`FEATURES.md`](FEATURES.md),
[`firmware/README.md`](firmware/README.md), and
[`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md).

---

## 1. What you need

**Hardware** — one of the two supported boards:

| Board | MCU | Codec | SLIC | Notes |
|-------|-----|-------|------|-------|
| **SMD** (`Rotary_phone_SMD2`) | ESP32-C6-WROOM-1 | ES8388 | AG1171 ×1–2 | Production PCB; 1 or 2 phone lines, optional microSD |
| **Through-hole** (`rotary_phone_gateway`) | ESP32-C6-DevKitM-1 | WM8731 | AG1170 ×2 | Reference/breadboard build |

Plus: a rotary or touch-tone **telephone** on tip/ring, a **USB-C cable**, and
a 2.4 GHz **Wi-Fi** network. (Hardware design files are under
[`hardware/`](hardware/).)

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

See [`firmware/README.md`](firmware/README.md) for a more detailed toolchain
walkthrough (including Windows).

---

## 3. Get the code

```bash
git clone <this-repo-url> esp32_rotary_phone
cd esp32_rotary_phone/firmware
```

All firmware lives under `firmware/`. You run `idf.py` from there.

---

## 4. Configure for your board (`idf.py menuconfig`)

The firmware is **one code tree** that builds for either board and several
hardware options. Pick yours under **menuconfig → Board configuration**:

```bash
idf.py menuconfig
```

| Option | Choose |
|--------|--------|
| **Target board type** | *SMD2* (ES8388) or *Through-hole* (WM8731) — match your PCB |
| **Number of active phone lines** | `1` or `2` (single-line boards leave line 2 unpopulated) |
| **SMD board uses microSD instead of line 2** | Enable **only** if your board has the microSD socket stuffed instead of the Line-2 SLIC (they share GPIO 18–21). Requires *lines = 1*. |

Save and exit. That's it — the pin map, codec driver, line count, and SD vs.
Line-2 routing all follow from these choices. (Wi-Fi and VoIP are **not** set
here; those are configured at runtime — see below.)

> **Note on committing config:** `sdkconfig` holds your board's choices. The
> repo's committed default is the **2-line, no-SD SMD** build. Your local
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

You should see the subsystems come up (`board`, `audio`, codec, `ag1170`,
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

Alternatively, from the serial console:
`wifi_set "<ssid>" "<password>"`.

Once connected, the log prints the device's IP and its mDNS name (default
**`phone.local`**). Open **`http://phone.local/`** (or the IP) in a browser.

> **Multiple phones on one network?** Give each a unique **Board name** under
> Settings → General; it becomes the `<name>.local` address and labels the UI.

---

## 7. The web UI

- **Status** — live line state, Wi-Fi, uptime, heap, storage.
- **Recordings** — messages left with dial code 211 (download as WAV).
- **Dial Guide** — the live dial plan (generated from firmware, always
  accurate). Cards that don't apply to your build (e.g. "Call other phone" on a
  1-line board) are hidden.
- **Settings** — board name, phone mode (play / call-only), digit readback,
  Wi-Fi, VoIP status, and a storage file manager. Hardware tab shows chip type,
  detected codec, serial (MAC), and flash/RAM/storage usage.

---

## 8. Optional setup

**Audio clips & prompts.** Two ways to get WAVs onto the phone:

- **Bundled at build time** — anything you drop in `firmware/assets/` gets
  baked into a dedicated `bundled` flash partition and reflashed with every
  `idf.py flash`, without ever touching recordings in the writable `storage`
  partition. Only `operator.wav` ships by default (original project text,
  not copyrighted); everything else is gitignored on purpose — the fun-clip
  test set (Ring of Fire, Crocodile Rock, ...) is copyrighted and doesn't
  belong in a public repo. Add your own, from audio you own the rights to:

  ```bash
  cd firmware/bringup
  ./sync_pi_audio.py --src /path/to/your/audio --out-dir ../assets   # batch
  ./gen_prompt.py --from song.mp3 --name clip_401.wav --out-dir ../assets  # one file
  ```

  See [`firmware/assets/README.md`](firmware/assets/README.md) for the full
  mechanism and override rules.

- **Uploaded at runtime** — `gen_prompt.py --upload phone.local` (or the web
  UI's Settings → Storage file manager) writes straight into the writable
  partition, no rebuild/reflash needed. A same-named upload always overrides
  a bundled default.

Well-known names: `operator.wav`, `clip_401.wav`…`clip_414.wav`. See
[`pi_rotary_phone/docs/DIAL_CODES.md`](pi_rotary_phone/docs/DIAL_CODES.md) for
what each code does.

**Custom dial codes.** To assign a *new* number to a clip (bundled or
uploaded) without touching the fixed dial-plan table or rebuilding, use
Settings → General → "Custom dial codes": pick a 2–6 digit code and any
already-available WAV from the dropdown. Stored in flash (NVS), takes effect
immediately.

**VoIP (SIP).** To register to a LAN Asterisk/PBX and place calls (dial
`555` + a 4-digit extension), configure from the console:

```
sip_set <server> <user0> <pass0> [user1] [pass1]
```

then reboot. Check with `sip_status`. LAN only (no TLS/SRTP).

**microSD.** If you built with microSD enabled, the boot log shows whether the
card mounted; `sd_test` from the console prints card info and runs a
write/read/verify.

---

## 9. Resetting Wi-Fi (factory reset)

With the board **powered on and running**, short **GPIO9 (the BOOT strap)** to
GND (or hold the BOOT button) for **~3 seconds**. It erases the saved Wi-Fi
credentials and reboots into the setup AP (section 6).

> Do this at **runtime, not at power-on** — GPIO9 held low *during reset* just
> enters the chip's USB flashing mode.

---

## 10. Where to go next

| Doc | For |
|-----|-----|
| [`firmware/README.md`](firmware/README.md) | Toolchain detail, console command reference, build layout |
| [`FEATURES.md`](FEATURES.md) | What's implemented + firmware architecture |
| [`ESP32_PORT_PLAN.md`](ESP32_PORT_PLAN.md) | Roadmap, module map, future ideas |
| [`hardware/`](hardware/) | Schematics, BOMs, PCB clearance rules |
| `pi_rotary_phone/docs/DIAL_CODES.md` | Full dial-plan reference |

Questions or a board that behaves oddly? The serial `help` console is the
fastest way to poke at hardware (hook, ring, DTMF, audio, SD, SIP) directly.
