[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · [3 · Flash & test](flashing.md) · **4 · Set up & use**

# 4 · Set up & use

Getting a flashed board onto your Wi-Fi, and everything you can do from its
web interface. If someone handed you a ready-made board, start here.

## 1. Get it on your Wi-Fi

Power the board up. With no Wi-Fi saved yet, it makes **its own Wi-Fi
network** for setup:

1. On your phone or laptop, join the open network **`AnalogRedial-XXXX`**.
2. A setup page should pop up. If it doesn't, browse to
   **`http://192.168.4.1`**.
3. Enter your home Wi-Fi name and password, and optionally give the board
   a name. Submit, and it reboots onto your network.

The board needs **2.4 GHz** Wi-Fi; the ESP32-C6 can't use 5 GHz.

The phones work before any of this: dial tone, calls between the two lines,
clips and recordings all run without a network. Wi-Fi is for the web
interface and VoIP.

## 2. Open the web interface

Browse to **`http://phone.local`** from any device on the same Wi-Fi. If you
named the board, it's `http://<name>.local` instead.

- **More than one board?** If `phone` is already taken on the network, the
  board uses `phone1`, `phone2` and so on. Give each board its own name in
  Settings → General to keep it fixed.
- **`.local` not loading?** Some networks don't pass these names. Find the
  board in your router's list of connected devices and open its IP address
  directly.
- **There's no login.** Anyone on your Wi-Fi can open the settings, so keep
  the board on a network you trust.

## 3. Dialing

The **Status** page always shows the board's current dial codes. The
defaults:

| Dial | What happens |
|------|--------------|
| **0** | Operator: a spoken help message |
| **211** | Record a message after the beep. Play or download it under Recordings |
| **321** | Ring the other phone (two-line boards) |
| **322**, **323**, **324** | Ring the other phone, talking as a chipmunk, Darth Vader or a Dalek |
| **412** | A dial-up modem, for old times' sake |
| **555** + 4 digits | A VoIP call to that extension ([VoIP setup](voip-setup.md)) |
| **821** / **822** | Digit readback off / on |
| **899** | Record this line's answering-machine greeting |

Add your own codes with your own audio in Settings → Dial Codes & Prompts.

**During a call**, with voice effects turned on in Settings → Phone
Settings, press or dial a digit to change both voices: **1** Dalek,
**2** chipmunk, **3** Darth Vader, **4** 8-bit, **5** robot, **6** tremolo,
**7** distortion, **0** back to normal. The other person hears the key tone
too.

## 4. The web interface

Click any picture for the full page.

<table>
  <tr>
    <td width="40%"><a href="pics/status.png"><img src="pics/thumbs/status.png" alt="Status page"></a></td>
    <td valign="top"><b>Status.</b> Each line's state and last caller, Wi-Fi
    and storage, recent activity (calls, recordings, misdials) and the dial
    guide. On a new board it also shows a one-time setup card for the board
    name and hardware type.</td>
  </tr>
  <tr>
    <td><a href="pics/recordings.png"><img src="pics/thumbs/recordings.png" alt="Recordings page"></a></td>
    <td valign="top"><b>Recordings.</b> Messages left with 211 and on the
    answering machine: length, time, play, download, delete.</td>
  </tr>
  <tr>
    <td><a href="pics/settings-general.png"><img src="pics/thumbs/settings-general.png" alt="General settings"></a></td>
    <td valign="top"><b>Settings → General.</b> Board name, saved Wi-Fi
    networks, interface language, and quiet hours that keep the bell silent
    overnight.</td>
  </tr>
  <tr>
    <td><a href="pics/settings-phone.png"><img src="pics/thumbs/settings-phone.png" alt="Phone settings"></a></td>
    <td valign="top"><b>Settings → Phone Settings.</b> The answering machine
    per line, your country's dial tone, the off-hook alert, echo
    cancellation, digit readback, voice effects, intercom mode and earpiece
    volume.</td>
  </tr>
  <tr>
    <td><a href="pics/settings-dialcodes.png"><img src="pics/thumbs/settings-dialcodes.png" alt="Dial codes and prompts"></a></td>
    <td valign="top"><b>Settings → Dial Codes & Prompts.</b> Every clip code
    and its audio, editable: change a code, upload new audio, or add a new
    one. Also the operator message and the record-message prompt.</td>
  </tr>
  <tr>
    <td><a href="pics/settings-voip.png"><img src="pics/thumbs/settings-voip.png" alt="VoIP settings"></a></td>
    <td valign="top"><b>Settings → VOIP.</b> SIP server and per-line
    extension, registration status, and a generated setup script for your
    Asterisk server. Walkthrough in §6 below.</td>
  </tr>
  <tr>
    <td><a href="pics/settings-hardware.png"><img src="pics/thumbs/settings-hardware.png" alt="Hardware settings"></a></td>
    <td valign="top"><b>Settings → Hardware.</b> One or two lines,
    microSD, board and chip details, firmware updates, and resets.</td>
  </tr>
  <tr>
    <td><a href="pics/settings-storage.png"><img src="pics/thumbs/settings-storage.png" alt="Storage settings"></a></td>
    <td valign="top"><b>Settings → Storage.</b> A file manager for the
    board's flash and the microSD card.</td>
  </tr>
  <tr>
    <td><a href="pics/settings-diag.png"><img src="pics/thumbs/settings-diag.png" alt="Diagnostics"></a></td>
    <td valign="top"><b>Settings → Diagnostics.</b> The full log, log
    level, and the hardware self-test.</td>
  </tr>
</table>

**Settings → Bluetooth** arrives with the v5 board.

## 5. Your own audio

**From the browser:** Settings → Dial Codes & Prompts takes a WAV upload in
almost any format (any sample rate, 8 to 32 bit, mono or stereo) and
converts it on the board. Changes take effect immediately, no rebuild.

**From the command line:** `firmware/bringup/gen_prompt.py` makes a clip
from an MP3 or from text (text-to-speech on your computer) and uploads it.
To build your own clips into every firmware image instead, see
[`firmware/assets/README.md`](https://github.com/sdylewski/Analog-Redial/blob/main/firmware/assets/README.md).

Use audio you have the rights to. The board ships with the operator message,
the answering machine's default greeting and a public-domain modem sound.

## 6. VoIP

Optional. Register one or both lines with Asterisk, FreePBX or another SIP
server to call beyond the board: fill in Settings → VOIP, download the
generated setup script, run it on your server, and dial **555** + an
extension. Calls stay on your local network (no encryption).
Walkthrough: [VoIP setup](voip-setup.md).

## 7. The status light

The DevKit's RGB LED shows what the board is doing. It's deliberately dim.

| Light | Meaning |
|---|---|
| **Red, fast blink** | Starting up |
| **Red, slow blink** | Something failed to start, but the board is running. Check Settings → Diagnostics |
| **Green, slow blink** | Joining Wi-Fi, or waiting in setup mode. If it keeps blinking after boot, it's in setup mode: see [§1](#1-get-it-on-your-wi-fi) |
| **Green, solid** (30 s) | Just joined Wi-Fi |
| **Off** | Running normally |
| **Yellow, fast blink** | Installing a firmware update. Don't unplug it |
| **Blue** | Bluetooth pairing (v5) |

## 8. The BOOT button: rescue and reset

With the board **running**, hold the **BOOT** button on the DevKit:

| Hold | What happens |
|------|--------------|
| **3 seconds, then let go** | **Rescue mode.** The board brings up its own Wi-Fi network (named after the board) for one session, keeping your saved networks. Join it and browse to **192.168.4.1** for the full web interface. Use this when it can't reach the network in front of it |
| **10 seconds** | **Forget Wi-Fi.** Erases saved networks and restarts in setup mode ([§1](#1-get-it-on-your-wi-fi)) |

The light changes at the 3-second mark so you know when to let go. Holding
BOOT **while plugging in** does something different: it puts the chip in
USB flashing mode.

## 9. Updating the firmware

Build a new image ([step 3](flashing.md)) and either flash it over USB, or
upload the `rotary_phone_gateway.bin` from your build directory in Settings → Hardware → Firmware. The
board installs it and restarts; if the new version fails to start, it goes
back to the old one.

---

**Back to: [Home](../README.md)**
