<p align="center">
  <img src="docs/assets/logo.svg" alt="Analog Redial logo" width="160">
</p>

<h1 align="center">Analog Redial</h1>

<p align="center">
  <b>Bring old telephones back to life.</b><br>
  Plug in one or two analog phones, rotary or touch-tone, and they get real
  dial tone, real ringing, and a little phone system of their own.<br>
  No server, no app, no subscription.
</p>

<p align="center">
  <a href="#what-can-i-do-with-it">What it does</a> ·
  <a href="#set-it-up-from-your-browser">Web interface</a> ·
  <a href="#what-does-one-cost">Cost</a> ·
  <a href="#build-one">Build one</a> ·
  <a href="#community">Community</a>
</p>

<table>
  <tr>
    <td width="50%"><img src="docs/pics/smd4_board.jpg" alt="The Analog Redial board"></td>
    <td width="50%"><img src="docs/assets/photo-placeholder.svg" alt="The board in its 3D-printed case (photo coming soon)"></td>
  </tr>
  <tr>
    <td align="center">The board: an ESP32-C6, an audio codec, and a phone-line chip per jack</td>
    <td align="center">In its 3D-printed case (<a href="https://github.com/sdylewski/Analog-Redial/tree/main/hardware/3d-printed-case">print files</a>)</td>
  </tr>
</table>

---

## What can I do with it?

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/assets/use-intercom.svg" alt="Two analog phones on one board">
      <h3>Intercom</h3>
      Two phones, one board. Dial <b>321</b> and the other phone rings with its
      real bell. Works with no Wi-Fi and no internet at all. Houses, workshops,
      kids' rooms, a studio and its control room.
    </td>
    <td width="50%" valign="top">
      <img src="docs/assets/use-standalone.svg" alt="One phone playing clips and recording messages">
      <h3>A phone that does things</h3>
      Dial a code and something happens: play a clip, leave a message, reach the
      answering machine. Museum exhibits, art pieces, a wedding guestbook phone,
      a phone that tells kids jokes.
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/assets/use-voip.svg" alt="The board registered with a SIP server">
      <h3>VoIP</h3>
      Register a line with Asterisk, FreePBX or any SIP server and call anything
      that server can reach. If the server goes down, the two phones can still
      call each other.
    </td>
    <td width="50%" valign="top">
      <img src="docs/assets/use-bluetooth.svg" alt="The board paired with a mobile phone">
      <h3>Bluetooth <i>(coming soon)</i></h3>
      Pair your mobile and the rotary phone becomes its handset: the bell rings
      for incoming calls, lift to answer, dial out on the rotary dial. Arrives
      with the next board revision.
    </td>
  </tr>
</table>

### Why not just buy a VoIP adapter?

Off-the-shelf analog telephone adapters (ATAs) only understand touch-tone,
so a rotary phone can't dial through them, and they need a SIP server before
they do anything at all, even to let two phones on the same box call each
other. Analog Redial reads rotary pulses as well as touch-tone, and it is a
complete phone exchange on its own.

### The finer print

- **Dialing.** Rotary pulse and touch-tone. Add your own dial codes and
  audio clips from the browser, no rebuild. Optional digit readback.
- **Calls.** Line-to-line calls with ringback and busy tones. Optional
  intercom mode: lift one handset, wait, and the other phone rings.
  Experimental voice effects mid-call: Dalek, chipmunk, Darth Vader, 8-bit,
  robot and more. VoIP in and out (G.711, on your local network).
- **Sound.** Dial tone, ringback and busy that match your country (North
  America, several European countries, Japan, China, or a generic
  international set). Echo cancellation and earpiece volume per line. An
  off-hook alert if a handset is left off.
- **Messages.** Record a message with **211** and download it from the
  browser. A per-line answering machine, each line with its own greeting.
  Fit a microSD card for hours of storage.
- **Day to day.** Quiet hours that silence the bell overnight. Remembers
  several Wi-Fi networks. Firmware updates from the browser. A one-button
  rescue mode for when it can't find its network.
- **Open.** ESP-IDF C firmware under the MIT license, KiCad hardware files,
  and a serial console for poking at every part of the hardware.

Full feature list and firmware architecture: [Features](docs/features.md).

---

## Set it up from your browser

The board serves its own web page at **`http://phone.local`**, reachable
from any phone or laptop on your Wi-Fi. No app, no account, no cloud. On
first boot it makes its own Wi-Fi network with a setup page that gets it
onto yours.

<table>
  <tr>
    <td align="center" width="33%"><a href="docs/pics/status.png"><img src="docs/pics/thumbs/status.png" alt="Status page"></a><br><b>Status</b><br>Lines, activity, the dial guide</td>
    <td align="center" width="33%"><a href="docs/pics/settings-phone.png"><img src="docs/pics/thumbs/settings-phone.png" alt="Phone settings"></a><br><b>Phone settings</b><br>Answering machine, tones, effects</td>
    <td align="center" width="33%"><a href="docs/pics/settings-dialcodes.png"><img src="docs/pics/thumbs/settings-dialcodes.png" alt="Dial codes and prompts"></a><br><b>Dial codes</b><br>Your own codes and audio</td>
  </tr>
  <tr>
    <td align="center"><a href="docs/pics/settings-voip.png"><img src="docs/pics/thumbs/settings-voip.png" alt="VoIP settings"></a><br><b>VoIP</b><br>SIP server, generated Asterisk script</td>
    <td align="center"><a href="docs/pics/recordings.png"><img src="docs/pics/thumbs/recordings.png" alt="Recordings"></a><br><b>Recordings</b><br>Play or download messages</td>
    <td align="center"><a href="docs/pics/settings-hardware.png"><img src="docs/pics/thumbs/settings-hardware.png" alt="Hardware settings"></a><br><b>Hardware</b><br>Line count, microSD, firmware update</td>
  </tr>
</table>

Click any picture for the full page. A guided tour is in
[Set up & use](docs/setup.md).

---

## What does one cost?

Building a single-line board yourself, US prices from October 2026
including shipping and import tariff:

| | Just you | Pooled order (30 boards) |
|---|---:|---:|
| Assembled PCB from JLCPCB (5-board minimum) | $85.53 for 5, so $17.11 each | about $6 each |
| Hand-solder parts from DigiKey ([parts list](docs/bom.md)) | about $19, plus shipping | about $19 |
| **One single-line board** | **about $110 up front**, with 4 spare PCBs | **about $25** |
| Second phone line (optional) | +$10 | +$10 |

You'll also want a 2 A USB-C charger and one or two analog phones. Buying
the hand-solder parts from AliExpress instead brings them to about $12 a
board.

**A pooled order makes boards much cheaper.** Most of a small order is fixed
setup fees, so the price per board falls fast with quantity. If you'd like a
board from a pooled order, register your interest: no commitment, it just
tells us how many to order. **Interest form: coming soon.**
<!-- TODO: replace "coming soon" with the interest form link -->

Price breakdown by quantity: [Get a board](docs/board.md#what-it-costs).

---

## Build one

No surface-mount soldering needed: the fab places the small parts, and
everything you solder is through-hole. If you can solder a header and type a
few commands in a terminal, you can build one.

| Step | Guide | What's in it |
|---|---|---|
| **1** | [Get a board](docs/board.md) | Board versions, ordering from JLCPCB, pooled orders, costs |
| **2** | [Parts](docs/bom.md) and [assembly](docs/building.md) | What to buy, soldering order, checks before power-on |
| **3** | [Flash & test](docs/flashing.md) | Install ESP-IDF, flash the firmware, run the hardware self-test |
| **4** | [Set up & use](docs/setup.md) | Wi-Fi, the web interface, dial codes, your own audio |

Going further: [VoIP setup](docs/voip-setup.md) ·
[Board reference (v4)](docs/hardware-smd4.md) ·
[Board reference (v5)](docs/hardware-smd5.md) ·
[Features & firmware architecture](docs/features.md) ·
[Firmware developer notes](https://github.com/sdylewski/Analog-Redial/blob/main/firmware/README.md)

> ⚠️ **Phone lines carry real voltage.** The board drives about 48 V DC on
> an off-hook line and about 60 V AC while ringing, with peaks near 90 V.
> Don't touch the phone jacks or the line chips while it's powered, and never
> connect it to your house or telephone-company wiring. This is a hobby
> project, not certified telecom equipment.

---

## Community

- **Discord:** coming soon <!-- TODO: Discord invite link -->
- **Bugs and questions:** [GitHub Issues](https://github.com/sdylewski/Analog-Redial/issues)
- **Built one?** Open an issue with a photo. We'd love to see it.

---

## Project history

| When | Version | What it was |
|------|---------|-------------|
| Aug 2020 | **Raspberry Pi** <br><img src="docs/pics/pi_version.jpg" width="220" alt="Raspberry Pi version in its project box"> | A Raspberry Pi 4 in a project box, two SLIC modules hand-wired to its GPIO header, and a USB sound dongle per phone line. Python software, after a short Arduino experiment in July 2020. |
| Jun 2026 | **v1** | The first ESP32 board: through-hole, with a WM8731 codec and two AG1170 SLICs. The Pi's job on a $5 microcontroller. |
| Jul 2026 | **v2** <br><img src="docs/pics/smd2_board.jpg" width="220" alt="Analog Redial v2 board"> | First surface-mount board: ESP32-C6 module, ES8388 codec, AG1171 SLICs, microSD. Three hand-soldered. |
| Sep 2026 | **v4** <br><img src="docs/pics/smd4_board.jpg" width="220" alt="Analog Redial v4 board"> | The current board: 2-layer, built around an ESP32-C6-DevKitC-1, one or two phone lines, optional microSD. |
| Oct 2026 | **v5** | At the fab: takes a genuine or clone ESP32-C6 module, and adds the Bluetooth companion. |

## License

[MIT](LICENSE) for the firmware and documentation. Hardware design files are
provided as-is for personal and hobby use; see the note in [`LICENSE`](LICENSE).
