# Hackaday.io page: "Analog Redial: a hackable analog phone system"

Paste-ready text for the hackaday.io project. Written about the **v5 board
with the BM64 Bluetooth option**, in your voice. Don't publish until v5 and
the BM64 carrier are tested; every spot that depends on that, or that only
you can fill in, is marked **[YOU]**.

Hackaday.io's editor changes now and then, so check the field names against
its "Create project" page. The Details and log editors take Markdown; paste,
then check the preview.

---

## 1. How to create the page

1. Sign in at hackaday.io, then **Create → Project**.
2. Fill in **Name**, **Summary**, **Tags** (section 2), upload the **cover
   image**, save. It stays unpublished until you publish it, so you can
   build the whole thing quietly.
3. Paste the **Details** (section 4). Add **Components** (5), **Links** and
   **Files** (6).
4. Add the **Instructions** (7) as build steps, a picture per step where you
   have one.
5. Post the **Project logs** (8). Logs are dated posts: put the first one up
   with the page, then add more as you go. They're what makes people follow.
6. Check it from a private window, publish, and link it from the README.

---

## 2. Name, summary, tags

**Name:** Analog Redial: a hackable analog phone system

Alternatives: *Building a hackable analog phone system* · *Analog Redial:
make old phones ring again*

**Summary** (Hackaday limits it to 140 characters; this is 136):

> Old rotary phones get dial tone, dial codes and an answering machine. With Bluetooth, your rotary phone becomes a handset for your cell.

Alternatives, also under 140: *Rotary phones get dial tone, dial codes and an
answering machine. Add Bluetooth: dial out on the old phone, ring the bell
for cell calls.* (137) · *Open-source phone system for old rotary/touch-tone
phones: dial codes, answering machine, VoIP, and Bluetooth to your cell
phone.* (129)

**Tags:** `esp32`, `esp32-c6`, `rotary phone`, `telephony`, `bluetooth`,
`voip`, `sip`, `slic`, `kicad`, `esp-idf`, `retro`, `claude code`

---

## 3. Images

| Use | File | Caption |
|-----|------|---------|
| **Cover** (landscape) **[YOU]** | a photo of the yellow wall phone plugged into the board | Yes, that's the actual phone from when I was a kid |
| v5 board, assembled **[YOU]** | photo after bring-up | The v5 board |
| v5 board (until you have a photo) | `docs/pics/V5_board_front.png` | v5 front |
| v5 board back | `docs/pics/V5_board_rear.png` | Solder jumpers for the genuine or clone ESP32-C6 module |
| Bluetooth carrier **[YOU]** | photo of the BM64 board on the header | The BM64 Bluetooth board plugs onto the 8-pin header |
| Web UI | `docs/pics/status.png` | Status page: lines, activity, the dial guide |
| Web UI | `docs/pics/settings-dialcodes.png` | Add your own dial codes and audio from the browser |
| Web UI | `docs/pics/settings-bluetooth.png` | Pairing your cell phone |
| Schematic | `docs/pics/smd5_schematic.png` | The v5 schematic |
| History | `docs/pics/pi_version.jpg` | The original Raspberry Pi version |
| History | `docs/pics/smd4_board.jpg` | v4, the board that's been running the house |

Hackaday crops the cover to a wide card: landscape, subject in the middle.
The yellow phone is the picture people will click on.

---

## 4. Details (paste this)

**Analog Redial turns old analog phones into a little phone system you can
hack.** Plug in one or two phones, rotary or touch-tone, and you get real dial
tone, ringing, calls between the phones, an answering machine, and a bunch of
dial codes that do fun things... all set up from a web page. Add the Bluetooth
board and your rotary phone becomes a handset for your cell phone!

Everything's open: KiCad hardware, ESP-IDF firmware in C (MIT license), and a
serial console for poking at every part of the hardware.

### Why I built it

This project started as a way to use my family's yellow rotary-dial wall
phone from when I was a child. One rotary phone could call another, and my
kid liked playing with it. The first version ran on a Raspberry Pi and worked
OK for years, but this ESP32-based version has more features and is cheaper
to build.

Why not just buy a VoIP adapter? Most of them only understand touch-tone, so
a rotary phone can't dial through them, and they need a SIP server before
they'll do anything, even let two phones on the same box call each other.
This is a whole phone system on its own.

### The best part: Bluetooth

Plug the Bluetooth board (a BM64 module) onto the header and pair it with
your cell phone. Then:

- **Make calls from the rotary phone.** Pick it up and dial 1 + area code +
  number. The call goes out on your cell phone, and the audio comes through
  the old handset.
- **Incoming calls ring the real bell.** Lift the handset to answer, hang up
  to end the call.
- **Caller ID** shows up in the web page's activity log.
- **Everything else still works.** With your cell connected but no call up,
  picking up still gives you dial tone and all the local dial codes.

There's also a cheaper Bluetooth module, the BK3266, that fits the same
header. It should ring and answer, but probably can't dial out. I'm still
testing it.

**[YOU: once tested, add a line on how well it works, e.g. call quality.]**

### Dial anything

The ESP32 sees every digit you dial, so any number can do whatever you want.
The dial plan is just a table. Out of the box:

| Dial | What happens |
|------|--------------|
| **321** | Ring the other phone |
| **211** | Record a message (download it from the web page later) |
| **0** | Operator help |
| **899** | Record your answering machine greeting |
| **412** | A dial-up modem sound, because why not |
| **1** + area code + number | Call out through your cell phone (Bluetooth) |

Add your own codes and audio files from the browser, no reflashing: a joke
line for the kids, a song, a museum exhibit, a wedding guestbook phone...

**Make a dial code switch something.** Two phone lines, or one line and four
spare pins, your choice. On a one-line board the pins for the second line
are wired out to ordinary 2.54 mm header pins, so you can tap in and do what
you want. In the web page, pick a code, a pin and a time: dial it and the pin
goes high for that many seconds (or toggles). Hook a relay or a MOSFET to it
to unlock a door, sound a buzzer, flash a light. (A solenoid needs more
current than a pin can give, so switch it through the relay.) The catch: the
same four pins are the microSD socket, so a board used this way has no card.

### More things it does

- **Rotary and touch-tone.** Both work on either line.
- **Answering machine** per line, each with its own greeting. Messages go to
  a microSD card.
- **Intercom mode:** lift one handset, wait a few seconds, and the other
  phone rings.
- **VoIP:** register a line with Asterisk or FreePBX and call anything that
  server can reach.
- **Proper phone sounds:** dial tone, ringback and busy tones for your
  country, plus echo cancellation.
- **Day to day:** quiet hours so the bell doesn't go off at 2 a.m., several
  remembered Wi-Fi networks, firmware updates from the browser, and an
  optional web password.

### How it works

Each phone line goes through the same chain:

```
phone ── RJ12 ── AG1171 SLIC ── ES8388 codec ── ESP32-C6 ── Wi-Fi / Bluetooth
```

- **The SLIC** (Silvertel AG1171) is what makes a phone think it's on a real
  phone line: it supplies the loop current, makes the ring voltage, and tells
  the ESP32 when the handset is lifted.
- **The codec** (ES8388) turns handset audio into digital audio and back. One
  codec handles both lines, as left and right channels.
- **The ESP32-C6** does the rest: it counts the pulses from a rotary dial,
  hears touch-tones, makes all the tones and the ring cadence, connects the
  two lines, and runs the web page and VoIP.
- **The Bluetooth board** connects straight to line 1's audio, so a cell
  phone call never goes through the ESP32 at all. The ESP32 just tells it
  what to do.

### The board (v5)

A 2-layer board around an ESP32-C6 DevKit, the ES8388 codec, and one or two
AG1171 SLICs. The fab places the surface-mount parts; you solder the DevKit,
SLICs, jacks and a few headers.

- **Genuine or cheap ESP32-C6 module.** The footprint takes either the real
  Espressif DevKitC-1 or a cheaper clone; a couple of solder jumpers on the
  back select which.
- **Bluetooth header** with its own 3.3 V regulator, for the BM64 board.
- **Second phone line or microSD** (they share four pins), picked with a
  header and a setting.

**[YOU: confirm these once v5 is tested.]**

### Setup

No app and no account. On first boot the board makes its own Wi-Fi network
with a setup page; pick your Wi-Fi, and after that it's at
`http://phone.local` from any phone or laptop in the house.

### Built with Claude Code

I wrote a lot of this with Claude Code, an AI coding tool. The web UI is the
part that really shows it: eight settings tabs, a file manager for your audio
and recordings, browser firmware updates, a VoIP number provisioner. For a
one-person hobby project that used to be way more work than I'd ever have time
for, so a project like this would have stopped at "it rings." Now it can have
the nice parts too.

### What it costs

Ordering the minimum five assembled boards from JLCPCB comes to about **$86
delivered (about $17 a board)**, plus about **$19 of parts you solder
yourself** for each board you build. **[YOU: re-check for v5 and add the
BM64 module and carrier cost.]**

If a few people want boards, ordering together gets a lot cheaper. **Want
one? [Fill in the interest form](https://forms.gle/kVKHiXejV6M2ZxC17)**: no
commitment, it just tells me how many to order.

### Get involved

Code, hardware and docs: **[GitHub: sdylewski/Analog-Redial](https://github.com/sdylewski/Analog-Redial)**

---

## 5. Components

| Qty | Part | What it's for |
|----:|------|---------------|
| 1 | ESP32-C6-DevKitC-1 (or a clone) | The brains: Wi-Fi, web page, dial plan, VoIP |
| 1 | ES8388 audio codec | Phone audio in and out |
| 1–2 | Silvertel AG1171 SLIC | The phone line: loop current, ring, hook detect |
| 1–2 | RJ12 jack | One per phone |
| 1 | 5 V DC supply and jack | Power |
| 0–1 | microSD socket and card | Message storage (one-line build) |
| 0–1 | BM64 Bluetooth module on its carrier board | Pair with your cell phone |
| 1 | XC6206 3.3 V regulator | Powers the Bluetooth board |
| — | Passives, TVS diodes, headers | Full BOM in the repo |

Full parts list with DigiKey numbers:
[`docs/bom.md`](https://github.com/sdylewski/Analog-Redial/blob/main/docs/bom.md).

---

## 6. Links and files

- GitHub: https://github.com/sdylewski/Analog-Redial
- Docs, start at "Get a board": https://github.com/sdylewski/Analog-Redial/blob/main/docs/board.md
- Interest form: https://forms.gle/kVKHiXejV6M2ZxC17
- KiCad library: https://github.com/sdylewski/MajorHack-Kicad-Libraries

**Files [YOU]:** the v5 Gerbers and BOM/CPL, the schematic PDF, and the
Bluetooth carrier's files, exported from the revision you tested.

---

## 7. Instructions (build steps)

1. **Get a board.** Order v5 from JLCPCB with SMT assembly (five-board
   minimum), or [fill in the interest form](https://forms.gle/kVKHiXejV6M2ZxC17)
   for a group order.
2. **Order the hand-solder parts.** The ESP32-C6 DevKit, SLICs, jacks and a
   few through-hole parts, about $19 from DigiKey (there's a ready-made list).
3. **Set the module jumpers.** Genuine Espressif DevKit: leave them alone.
   Cheaper clone: cut and re-bridge the two jumpers on the back.
4. **Assemble.** Solder the through-hole parts. Pick a second phone line or
   microSD: they share four pins, and a header selects.
5. **Check before power.** No shorts between the supply rails; the assembly
   guide has the checks.
6. **Flash.** Install ESP-IDF, build and flash over USB.
7. **Self-test.** A guided hardware check runs from the web page.
8. **Wi-Fi.** Join the board's setup network and pick yours.
9. **Plug in phones.** Lift a handset: dial tone! Dial 321 to ring the other.
10. **Bluetooth (optional).** Plug the BM64 board onto the header (pin 1 at
    the 3.3 V end), choose it in Settings → Hardware, run the one-time
    module check, then pair your cell phone from Settings → Bluetooth.
    **[YOU: describe the one-time BM64 setup as you actually do it; it
    needs two jumper wires to J5 today.]**

---

## 8. Project logs (post one at a time)

### Log 1: The yellow wall phone

This started with my family's yellow rotary-dial wall phone from when I was a
kid. I wanted it working again, and I wanted one rotary phone to be able to
call another. My kid thought that was the best thing ever.

The first version (2020) was a Raspberry Pi 4 in a project box, two phone
line modules hand-wired to the GPIO header, and a USB sound dongle per line,
with Python and shell scripts holding it together. No web page, but it worked
for years.

Then I wanted a board anyone could build:

- **v1:** through-hole, a different codec. Never built: the boards got lost in
  the mail.
- **v2:** first surface-mount board on an ESP32-C6. Needed some rework on the
  audio and USB, but it worked.
- **v4:** a cheaper 2-layer board. This one's been running the house.
- **v5:** takes a genuine or clone ESP32-C6, and adds Bluetooth.

*Images: `pi_version.jpg`, `smd4_board.jpg`, v5 photo.*

### Log 2: Dial anything

The fun part of building your own phone system: the ESP32 sees every digit,
so a number can do anything. The dial plan is a table that maps a code to an
action: play a clip, record a message, ring the other phone, call out over
VoIP or Bluetooth. The web page's dial guide is generated from the same table,
so it can't get out of sync with what the phone actually does.

You add your own codes and audio from the browser. Upload a WAV (any sample
rate, it gets converted; MP3 too if you turn that on), pick a number, done.
No reflashing.

Some ideas: a joke line for the kids, a "press 1 for..." menu for a museum
exhibit, a wedding guestbook phone, a phone that plays a song from the year
you dial.

**[YOU: if the spare-pin action is in by then, a door-unlock or buzzer demo
would make a great photo for this log.]**

### Log 3: Your cell phone, with a rotary dial

The BM64 Bluetooth module pairs with your cell phone like a headset. Its
audio is wired straight to line 1's phone chip, so the call never passes
through the ESP32; the ESP32 just sends it commands over a serial line.

- Dial 1 + area code + number on the rotary phone, and your cell places the
  call. The audio comes out of the old handset.
- An incoming call on your cell rings the real bell. Lift the handset to
  answer.
- Caller ID lands in the activity log.

The BM64 needed a one-time configuration change before its analog audio
worked at all (out of the box it sends audio to a digital output this board
doesn't use), so the board can check and fix that for you.

I also designed a board for a much cheaper module, the BK3266. Its command
set has no "dial this number," so it'll probably ring and answer but not
dial out. Testing it now.

**[YOU: results from your testing.]**

### Log 4: How a rotary dial talks to a microcontroller

A rotary dial doesn't send a tone. It opens and closes the phone line, about
ten times a second, once per click. The SLIC turns that into a logic signal
for the ESP32, which counts the clicks; a longer pause means the next digit.

Touch-tone is the opposite: it's sound. The ESP32 listens to the line's audio
for the eight touch-tone frequencies. Either way the digits land in the same
buffer, so the dial plan doesn't care which kind of phone you have.

---

## 9. Before you publish

- [ ] v5 brought up and working **[YOU]**
- [ ] BM64 board tested: pairing, dial-out, incoming ring **[YOU]**
- [ ] Pin dial code tried on the bench with a relay or an LED **[YOU]**
- [ ] Photos: the yellow phone plugged in, the v5 board, the Bluetooth board
- [ ] Re-check the cost line for v5, and add the Bluetooth parts
- [ ] Attach the tested revision's files
- [ ] GitHub repo is public, and the README links to the Hackaday page
- [ ] Open the page in a private window and click every link

## 10. Open questions

1. **Spare pins are now a feature.** The four pins are GPIO18-21 on header
   J5 (odd pins: 1 = GPIO21, 3 = GPIO20, 5 = GPIO19, 7 = GPIO18), driven
   from Settings -> Dial Codes -> Pin outputs on a one-line board with no
   microSD. The two header pads J3/J4 are *not* used for this: they depend on
   which ESP32 module and jumper setting you have. A photo of a door-strike
   or LED hooked to J5 would make a good log post.
2. **Bluetooth dial-out format.** The firmware dials out on 1 + 10 digits
   today. Fine for the US; other countries would need a change.
