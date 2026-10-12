# Hackaday.io page: "Building a hackable analog phone system"

Paste-ready text for a hackaday.io project. Everything below is taken from
this repo's docs, so it matches what the project actually does today. It is
written in first person as you; change the voice wherever it doesn't sound
like you. Items marked **[YOU]** need something only you have (a photo, a
decision, a link).

Hackaday.io's editor changes from time to time, so treat the field names
below as a guide and check them against the "Create project" page. The Details
and log editors accept Markdown; paste, then look at the preview.

---

## 1. How to create the page

1. Sign in at hackaday.io, then **Create → Project** (the "+" or "Add
   project" button on your profile or the Projects page).
2. Fill in the **Name**, **Summary** and **Tags** (section 2), upload the
   **cover image**, and save. A new project starts unpublished, so you can
   build the whole page before anyone sees it.
3. Paste the **Details** (section 4). Add the **Components** (section 5),
   **Links** (section 6) and **Files**.
4. Add the **Instructions** (section 7) as build steps, with a picture per
   step where you have one.
5. Post the **Project logs** (section 8). Logs are dated posts; write the
   first one now, and add more as you go. They are what makes people follow
   a project.
6. Preview from a logged-out window, then publish. Post the link in the
   repo's README so the two point at each other.

The Details section is the page people read first; the logs and
instructions are where the depth goes.

---

## 2. Name, summary, tags

**Name** (pick one):

- **Analog Redial: a hackable analog phone system** *(recommended: says what
  it is and matches the repo)*
- Building a hackable analog phone system
- Analog Redial: a $6 phone exchange for rotary phones

**Summary** (one or two sentences; shown on cards and in search):

> An open-source phone exchange for old analog phones: an ESP32-C6, an audio
> codec and two SLICs give rotary or touch-tone phones dial tone, ringing, an
> answering machine and VoIP, configured from a web page.

Shorter alternative:

> Plug in rotary or touch-tone phones and give them dial tone, an answering
> machine and VoIP. ESP32-C6, open hardware and firmware.

**Tags:** `esp32`, `esp32-c6`, `rotary phone`, `telephony`, `voip`, `sip`,
`slic`, `asterisk`, `kicad`, `esp-idf`, `retro`, `analog`

---

## 3. Images

| Use | File in the repo | Suggested caption |
|-----|------------------|-------------------|
| **Cover** (landscape, shown on cards) | `docs/pics/smd4_board.jpg` | The Analog Redial board: an ESP32-C6, an audio codec and two phone-line chips |
| Board, annotated | `docs/pics/smd4_board_annotated.jpg` | Where everything is |
| Case **[YOU]** | not taken yet | The board in its 3D-printed case. *(the README still shows a placeholder here)* |
| Web UI: status | `docs/pics/status.png` | The Status page: lines, activity feed and the dial guide |
| Web UI: phone settings | `docs/pics/settings-phone.png` | Answering machine, tones, voice effects |
| Web UI: dial codes | `docs/pics/settings-dialcodes.png` | Your own dial codes and audio, no rebuild |
| Web UI: VoIP | `docs/pics/settings-voip.png` | SIP server setup and the generated Asterisk script |
| Schematic | `docs/pics/smd4_schematic.png` | The v4 schematic |
| PCB top / bottom | `docs/pics/smd4_layout_top.png`, `smd4_layout_bottom.png` | The 2-layer layout |
| History: the Pi version | `docs/pics/pi_version.jpg` | The 2020 Raspberry Pi version |
| History: first SMD board | `docs/pics/smd2_board.jpg` | v2, the first surface-mount board |
| Next board | `docs/pics/V5_board_front.png` | v5: takes a genuine or clone ESP32-C6, and adds Bluetooth |

Hackaday crops the cover to a wide card, so use a landscape picture with the
subject in the middle. A photo with an actual phone plugged in will beat the
bare board.

---

## 4. Details (paste this)

**Analog Redial is an open-source phone exchange for old analog
telephones.** Plug in one or two rotary or touch-tone phones and they get
real dial tone, ringing, an operator, an answering machine and calls to each
other, with a web page for configuration. If you want, they also register as
extensions on any SIP server (Asterisk, FreePBX) and call anything that
server can reach.

Everything is open: KiCad hardware, ESP-IDF C firmware (MIT license), and a
serial console for poking at every part of the hardware.

### Why I built it

**[YOU: put your own reason here, in a sentence or two.]** The off-the-shelf
answer for connecting an old phone is a VoIP adapter (an ATA), but those
have two problems for this:

- Most only understand **touch-tone**, so a rotary phone can't dial through
  them.
- They need a **SIP server** before they do anything at all, even to let two
  phones on the same box call each other.

My first version was a Raspberry Pi 4 in a project box, two SLIC modules
hand-wired to the GPIO header, and a USB sound card per phone line, running
hand-written Python and shell scripts. No web UI, but it worked. This
project is that job done on a $5 microcontroller.

### What it does

- **Rotary and touch-tone dialing.** Pulse dialing is read from the phone's
  hook switch; touch-tone from the audio.
- **Calls between the two phones, fully offline.** No Wi-Fi, no server.
  Dial **321**, or turn on intercom mode: lift one handset, wait, and the
  other rings.
- **Dial codes that do things.** Play a clip, leave a message, reach the
  answering machine. Add your own codes and audio from the browser, no
  rebuild. Good for museum exhibits, art pieces, a wedding guestbook phone, a
  phone that tells kids jokes.
- **Messages.** Dial **211** to record; a per-line answering machine with its
  own greeting; recordings go to a microSD card and download from the
  browser.
- **VoIP.** An embedded SIP user agent with RTP/G.711. Register a line with
  Asterisk or FreePBX; an optional provisioner service hands out numbers
  from the board's own web page.
- **Proper sounds.** Dial tone, ringback and busy tones that match your
  country, echo cancellation, per-line earpiece volume.
- **Day to day.** Quiet hours that silence the bell overnight, several
  remembered Wi-Fi networks, firmware updates from the browser, a one-button
  rescue mode, and an optional web password.

### How it works

Each phone line goes through the same chain:

```
phone ── RJ12 ── AG1171 SLIC ── ES8388 codec ── ESP32-C6 ── Wi-Fi ── SIP server
```

- **The SLIC** (Silvertel AG1171) is the part that makes a phone think it's
  on a telephone exchange: it supplies the loop current, generates the ring
  voltage, and tells the microcontroller when the handset is lifted.
- **The codec** (ES8388) turns the handset audio into digital audio over I²S,
  and back. One codec serves both lines as left and right channels.
- **The ESP32-C6** does everything else: it times the pulses a rotary dial
  makes, listens for touch-tones with a Goertzel detector, generates the tones
  and the ring cadence, mixes and bridges audio between the lines, and runs
  the SIP/RTP stack.

The firmware is split into components, so the telephony logic never touches
registers directly (`board`, `es8388`, `ag1170`, `audio`, `tones`, `dtmf`,
`telephony`, `dialplan`, `voip`, and the web UI components). The dial plan is
one data table, and the web UI's dial guide is generated from the same table,
so the guide can't drift from what the phone really does.

### The board

The current board (v4) is a 2-layer PCB built around an ESP32-C6-DevKitC-1,
the ES8388 codec and one or two AG1171 SLICs. Surface-mount parts are placed
by the fab; the DevKit, SLICs and jacks are hand-soldered. A board can carry
a **second phone line or a microSD slot**, not both (they share four pins),
chosen with a header and a setting.

### Setting it up

No app and no account. On first boot the board makes its own Wi-Fi network
with a setup page that gets it onto yours; after that it serves its web page
at `http://phone.local`.

### What it costs

Ordering from JLCPCB with SMT assembly, delivered to the US (October 2026,
including shipping and import tariff):

| | 5 boards | 30 boards |
|--|--:|--:|
| Per board, assembled | $17.11 | $6.14 |
| Plus the hand-soldered parts (about $19) | about $36 | about $25 |

### Status and what's next

- **Working:** everything above, on boards I use daily.
- **At the fab:** v5, which takes a genuine or clone ESP32-C6 module and adds
  an 8-pin header for a Bluetooth companion.
- **Experimental:** Bluetooth (pair a mobile phone and the rotary phone
  becomes its handset), in-call voice effects, intercom.
- **Not yet:** internet radio dial codes are reserved but not implemented.

### Get involved

- Code, hardware files and docs: **[GitHub: sdylewski/Analog-Redial](https://github.com/sdylewski/Analog-Redial)**
- Want a board from a pooled order? **[Interest form](https://forms.gle/kVKHiXejV6M2ZxC17)**: no
  commitment, it just tells me how many to order.

---

## 5. Components

| Qty | Part | What it's for |
|----:|------|---------------|
| 1 | ESP32-C6-DevKitC-1 | The microcontroller: Wi-Fi, firmware, SIP |
| 1 | Everest Semiconductor ES8388 audio codec | Audio in/out of the phones (I²S) |
| 1–2 | Silvertel AG1171 SLIC | Phone-line interface: loop current, ring, hook detect |
| 1–2 | RJ12 jack | Plug for each phone |
| 1 | 5 V DC supply and jack | Power |
| 0–1 | microSD socket + card | Message storage (one-line build) |
| — | Passives, TVS diodes, headers | See the BOM in the repo |

The full list with DigiKey part numbers and prices is in
[`docs/bom.md`](https://github.com/sdylewski/Analog-Redial/blob/main/docs/bom.md).

---

## 6. Links

- GitHub repository: https://github.com/sdylewski/Analog-Redial
- Documentation (start at "Get a board"): https://github.com/sdylewski/Analog-Redial/blob/main/docs/board.md
- Pooled order interest form: https://forms.gle/kVKHiXejV6M2ZxC17
- KiCad shared library: https://github.com/sdylewski/MajorHack-Kicad-Libraries

**Files** to attach: the Gerber zip and BOM/CPL for the current board, and
the schematic PDF. **[YOU]** export these from KiCad so the attached files
match the revision you tell people to order.

---

## 7. Instructions (build steps)

1. **Get a board.** Order the bare PCB with SMT assembly from JLCPCB (5-board
   minimum), or join a pooled order. See *Get a board* in the repo docs.
2. **Order the hand-solder parts.** The ESP32-C6 DevKit, the SLICs, jacks and
   a few through-hole parts, about $19 from DigiKey (a parts list is
   provided).
3. **Assemble.** Solder the through-hole parts. Choose one build: second
   phone line, or microSD (they share four pins; a 2×4 header selects).
4. **Check before power.** Resistance between the supply rails, polarity, and
   no shorts. (The assembly guide has the checks and the order.)
5. **Flash.** Install ESP-IDF, build, and flash over USB.
6. **Self-test.** The firmware has a guided hardware check from the web UI
   and a serial console, with one command per subsystem.
7. **Put it on Wi-Fi.** Join the board's own setup network and pick yours.
8. **Plug in phones.** Lift a handset: dial tone. Dial 321 to ring the other.
9. **Optional: VoIP.** Enter your SIP server; the web UI generates the
   Asterisk script, or claim a number from the provisioner.

---

## 8. Project logs (drafts; post one at a time, newest last)

### Log 1: From a Raspberry Pi in a box to a $6 board

In 2020 I built a phone exchange for the house: a Raspberry Pi 4 in a
project box, two SLIC modules hand-wired to the GPIO header, a USB sound
dongle per phone line, and hand-written Python and shell scripts. No web UI,
but it worked.

This year I wanted it as a board anyone could build. The history, so far:

- **v1 (June 2026):** through-hole, WM8731 codec, two AG1170 SLICs. Never
  built; the boards got lost in the mail.
- **v2 (July 2026):** first surface-mount board. ESP32-C6 module, ES8388,
  AG1171 SLICs, microSD. Needed rework on the audio lines and the USB
  connection, but it worked.
- **v4 (September 2026):** a 2-layer board around the cheaper 32-pin
  ESP32-C6-DevKitC-1.
- **v5 (October 2026):** at the fab now. Takes a genuine or clone ESP32-C6
  module, and adds a header for a Bluetooth companion.

*Images: `pi_version.jpg`, `smd2_board.jpg`, `smd4_board.jpg`*

### Log 2: How a rotary dial talks to a microcontroller

A rotary dial doesn't send a tone; it opens and closes the phone's loop
current, once per digit step, about ten times a second. The SLIC turns that
into a logic signal (SHK, "switch hook") that the ESP32 times: a digit is a
run of short interruptions, and a longer pause marks the gap between digits.
Pulse dialing is detected from that, with a debounce, and the same logic
also handles the handset being lifted and put down.

Touch-tone is the opposite: it's audio. The codec's input stream goes
through a Goertzel detector looking at the eight DTMF frequencies, with a
debounce and a short lockout on repeated digits. Both kinds of digit end up
in the same dial buffer, so everything after that, the dial plan, doesn't
care which phone you have.

### Log 3: A web interface that lives on a microcontroller

The board serves its own configuration site with no app and no cloud. The
interesting part was fitting it in an ESP32-C6's RAM.

With a microSD card mounted, the board has about 67 KB of heap free at idle.
My settings pages were each built whole, in one allocation, before being
sent: the Diagnostics page wanted about 50 KB in one piece. On the bench, two
tabs returned HTTP 500 outright, and loading a few pages at once drove the
heap's low-water mark to 4 KB.

The fix was to render pages through a fixed 4 KB buffer and send them as a
chunked response as it fills. The page-building code didn't change: the
function that appends to the page just flushes when the buffer is full. Pages
are byte-for-byte identical, the two broken tabs work, and the low-water mark
under the same load went from 4 KB to about 34 KB. I also added a small
watchdog that restarts the board if the web server ever stops answering,
because an appliance on a shelf shouldn't need a button press.

### Log 4: The VoIP side, and a number-handout service

Each line is a SIP account. The board has an embedded user agent with
RTP/G.711 and digest authentication, plus an Asterisk setup script generated
from the web page. To avoid typing numbers into every board, I wrote a small
provisioner that runs next to the PBX: a board claims a number from its own
web page using HMAC-signed requests and a per-board token. The board finds
the provisioner on the local network by mDNS.

*(Add a screenshot of `settings-voip.png`.)*

---

## 9. Before you publish

- [ ] A real photo of a phone plugged in, and one of the case **[YOU]**
- [ ] Check every number in the cost table against `docs/board.md` the day
      you publish (the fab quote is dated October 2026)
- [ ] Attach the Gerber/BOM files for the revision you recommend **[YOU]**
- [ ] Make sure the GitHub repo is public and the README links back to the
      Hackaday page
- [ ] Open the page in a private window and click every link
- [ ] Decide which of the 4 logs go up on day one (the history log is the
      best opener)

## 10. Questions for you

1. **Which board do you want to feature, v4 or v5?** I wrote it around v4,
   which is built and in use, with v5 as "at the fab". Once v5 is tested it's
   a one-paragraph change.
2. **Photos.** I can resize and crop the repo images to the sizes the page
   uses. I can't take the phone-plugged-in or case photos.
3. **Tone.** I wrote it plainly; tell me if you want it more story-like.
