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

Alternatives, also under 140: *Rotary phones get dial tone, dial codes, an
answering machine and Home Assistant. Add Bluetooth and it's a handset for
your cell.* (129) · *Rotary phones get dial tone, dial codes and an
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
| Overview | `docs/assets/setup-overview-top-docs.png` (or `-letter-portrait.png`) | Everything it connects to: one phone always, pick one of line 2 / microSD / your own gadgets, Wi-Fi, Home Assistant, Bluetooth |
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

This is the text as it stands on the live page (synced 2026-10-14). Edit the
page and this file together, or this file goes stale.

**Analog Redial turns old analog phones into a little phone system you can
hack.**
Plug in one or two phones, rotary or touch-tone, and you get real dial tone,
ringing, calls between the phones, an answering machine, and a bunch of dial
codes that do fun things... all set up from a web page. It can also talk to
Home Assistant, so a dial code can turn on a light, or the phone can ring when
your doorbell does. Add the Bluetooth board and your rotary phone becomes a
handset for your cell phone!

Everything's open: KiCad hardware, ESP-IDF firmware in C (MIT license), and a
serial console for poking at every part of the hardware.

### Why I built it

This project started as a way to use my family's yellow rotary-dial wall
phone from when I was a child. One rotary phone could call another, and my
kid liked playing with it. The first version ran on a Raspberry Pi and worked
ok for years, but this ESP32-based version has more features and is cheaper
to build.

Why not just buy a VoIP adapter? Most of them only understand touch-tone, so
a rotary phone can't dial through them, and they need a SIP server before
they'll do anything. This is a whole phone system on its own, with the
ability to hack and extend it.

### Dial anything

The ESP32 sees every digit you dial, so any number can do whatever you want.
The dial plan is just a table. Defaults:

| Number | What happens |
|--------|--------------|
| **321** | Ring the other phone, if enabled |
| **211** | Record a message (download it from the web page later) |
| **0** | Operator help (you set the message beforehand) |
| **899** | Record your answering machine greeting |
| **412** | A dial-up modem recording - why not? |
| **1** + (area code) + (number) | Call out through your cell phone (Bluetooth) |

Add your own codes and audio files from the browser, no reflashing: a joke
line for the kids, a song, a museum exhibit, a wedding guestbook phone
recorder...

### Bluetooth option

Plug the Bluetooth board (a BM64 module) onto the header and pair it with
your cell phone. Then:

- **Make calls from the rotary phone.** Pick it up and dial 1+(area
  code)+(number). The call goes out on your cell phone, and the audio comes
  through the old handset.
- **Incoming calls ring the real bell.** Lift the handset to answer, hang up
  to end the call.
- **Caller ID** shows up in the web page's activity log.
- **Everything else still works.** With your cell connected but no call up,
  picking up still gives you dial tone and all the local dial codes.

There's also a cheaper Bluetooth module, the BK3266, that fits the same
header. It should ring and answer, but probably can't dial out. I'm still
testing it.

### Home Assistant

No add-on, no cloud. Tick one box in the settings and the board announces
itself to Home Assistant over MQTT: each line's state, a "ring the phone"
button, a "new messages" count, and an event every time someone lifts the
handset or dials a code. You can add codes whose only job is to tell Home
Assistant, like "dial 42 for the porch light".

It also works with a plain webhook if you'd rather skip MQTT. It's
experimental: I've tested it against a Mosquitto broker, not yet against a
real Home Assistant, so tell me what breaks.

**[YOU: "tick one box" skips one step: the MQTT card also wants your
broker's address (and its user and password). Maybe "tick one box and give
it your MQTT broker's address".]**

### Make a dial code switch something

The board has four spare pins that do one job at a time, and a little header
(J5) with jumpers picks which. Jumpers on: they run the second phone line.
Jumpers off: they read a microSD card if one's in the slot, or they're free
for your own wires if not.

With your own wires, pick a code, a pin and a time on the web page: dial it
and the pin goes high for that many seconds (or toggles). Hook a relay or a
MOSFET to it to unlock a door, sound a buzzer, flash a light. A pin gives
3.3 V and only a few milliamps, so a solenoid needs a relay in between. Most
5 V relay modules will switch, but check yours.

### More things it does

- **Rotary and touch-tone.** Both work on either line.
- **Answering machine** per line, each with its own greeting. Messages are
  saved on the board or on a microSD card if enabled.
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
Phone - RJ12 - AG1171 SLIC - ES8388 codec - ESP32 - WIFI / Bluetooth
```

- **The SLIC** (Silvertel AG1171) is what makes a phone think it's on a real
  phone line: it supplies the loop current, makes the ring voltage, and tells
  the ESP32 when the handset is lifted.
- **The codec** (ES8388) turns handset audio into digital audio and back. One
  codec handles both lines, as left and right channels.
- **The ESP32-C6** does the rest: it counts the pulses from a rotary dial,
  hears touch-tones, makes all the (internationalized) tones and the ring
  cadence, connects the two lines, and runs the web page and VoIP.
- **The Bluetooth board** connects straight to the first phone's audio, so a
  cell phone call never goes through the ESP32 at all. The ESP32 just tells
  it what to do.

### The board (v5)

A 2-layer board around an ESP32-C6 DevKit, the ES8388 codec, and one or two
AG1171 SLICs. The fab places the surface-mount parts; you solder the DevKit,
SLICs, jacks and a few headers.

- **Genuine 32-pin or cheap 30-pin ESP32-C6 module.** The footprint takes
  either the real Espressif DevKitC-1 or a cheaper clone; a couple of solder
  jumpers on the back select which.
- **Bluetooth header** with its own 3.3 V regulator, for the BM64 board.
- **Second phone line, microSD, or your own gadgets** (they share four
  pins), picked with the J5 jumpers and a setting.

*(The live page also has the Home Assistant paragraph a second time here,
after the board section. It reads like a paste slip; one copy, above, is
enough, so it isn't repeated in this file. **[YOU: remove the duplicate on
the page.]**)*

### Setup

You don't need an app or account. On first boot the board makes its own Wi-Fi
network with a setup page; pick your Wi-Fi, and after that it's at
`http://phone.local` from any phone or laptop in the house. There's a
complete web interface to check the board's recordings, in addition to
extensive settings and options.

### Built with Claude Code

I wrote a lot of this with Claude Code. The web UI is the part that really
shows it: eight settings tabs, a file manager for your audio and recordings,
browser firmware updates, a VoIP number provisioner. For a one-person hobby
project, AI helped me to do things quickly like tone dialing, an advanced web
UI, and bluetooth connection... all based on my original hand-written python
code from the raspberry pi.

### What it costs

Ordering the minimum five assembled boards from JLCPCB comes to about **$86
delivered (about $17 a board)**, plus about **$19 of parts you solder
yourself** for each board you build. Bluetooth is $20 extra or a 2nd line is
$10 for another SLIC board.

If a few people want boards, ordering together gets a lot cheaper. **Want
one? [Fill in the interest form](https://forms.gle/kVKHiXejV6M2ZxC17)**: no
commitment, it just tells me if you want me to order a batch for people... if
there's enough interest, I'll find a way to put it on CrowdSupply or
something.

### 3d printed case

There's a .step file for the board to make your own case, but you can also
just print the ready-made case yourself from the files in the project.

**[YOU: the ready-made cases in the repo
(`hardware/3d-printed-case`) are for the v4 board, and v5 is 5 mm longer, so
say which board the case fits, or add a v5 case.]**

### Get more info or build one yourself

Code, hardware and docs:
**[GitHub: sdylewski/Analog-Redial](https://github.com/sdylewski/Analog-Redial)**

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
4. **Assemble.** Solder the through-hole parts. Choose what the four spare
   pins do with the J5 jumpers: second phone line (jumpers on), or microSD
   or your own gadgets (jumpers off).
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
- [ ] Home Assistant tried against a real instance, then drop "not yet
      against a real Home Assistant" from the page **[YOU]**
- [ ] The duplicate Home Assistant paragraph removed from the live page
      **[YOU]**
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
2. **Home Assistant is untested against a real instance.** The MQTT part is
   verified against Mosquitto on the bench (discovery, state, events, the
   ring button, switching it on and off); the YAML in
   `docs/home-assistant.md` and the entities' look in the UI are not.
3. **Bluetooth dial-out format.** The firmware dials out on 1 + 10 digits
   today. Fine for the US; other countries would need a change.
