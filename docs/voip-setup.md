[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md)

# VoIP setup

Analog Redial works completely offline as a two-phone exchange with no
server at all — dialing the other line on the same board just bridges the
two calls locally. This doc covers the **optional** step of registering a
line with a SIP server (Asterisk, FreePBX, or any standards-compliant PBX)
so the board can place and receive calls beyond the phones directly attached
to it.

## Two ways to use VoIP

**Between Analog Redial boards on the same network, no server.** Every board
announces itself on your Wi-Fi and finds the others. The first time a board is
on a network each line picks a free 4-digit number for itself (starting with 6
or 7; never 9, which Bluetooth dial-out uses). Dial another board's number
and its phone rings: a direct VoIP call, nothing to install, no internet. The
boards found, and each line's number, are on **Settings → VOIP → Board-to-board
calls**, where you can change a number. Boards you haven't numbered yet show up
as "not set up".

**Through a SIP server such as Asterisk or FreePBX.** Register each line with
the server, and other phones registered there (at home or away) can call it, and
it can call them, by dialing **`555` + the 4-digit extension**. The same four
digits a line uses for board-to-board calls are the default extension, so one
number works both ways: from another board on your network it is just `6065`;
from a phone on the server it is `555` + `6065`. The rest of this page is about
the server.

Board-to-board calls keep working if the server is down, and need no account
anywhere else.

## How a "phone number" works on the ESP32 side

This firmware uses one value for both the SIP username and the dialable
extension — whatever you type into **Settings → VOIP → "Line N
number (extension)"** *is* the phone number, and it's what gets created as the
Asterisk extension too.

## Dialing between lines on a SIP server

The VoIP dial pattern is **`555` + a 4-digit extension**, exactly 7 digits total. It fires the
instant the 7th digit lands, strips the `555` prefix, and dials the
remaining 4 digits. Pick a numbering scheme for your own extensions (e.g.
`1000`, `1001`, `1002`, ...) and keep a note of which numbers are in use on
your server — there's no central registry, your PBX's own config is the
source of truth.

First turn VoIP on: **Settings → VOIP**, tick **Enable VoIP** at the top and
reboot.

## Automatic setup

If you run the [VoIP provisioner](https://github.com/sdylewski/Analog-Redial/blob/main/server/voip-provisioner/README.md) next to Asterisk (one install script),
each board claims its own number from its web interface:

1. Settings → VOIP → **Automatic setup**: press **Find on network** (or type
   the provisioner's address) and paste the enrollment key the installer
   printed. **Save**.
2. Type the number you want, press **Check**, then **Register**. The board
   restarts and connects with that number.

**Release** gives the number back. Requests are signed, so the key never
crosses the network; see the provisioner's README for how it stays secure
and for running it on the internet.

## Trying it without a PBX

For a bench test from a laptop on the same network,
`firmware/bringup/tiny_sip_proxy.py` is a small SIP registrar and proxy with no
dependencies (Python 3). Run it, set the board's SIP server (Settings → VOIP)
to the laptop's address and reboot the board, then register a softphone to the
same address (any username, no password, UDP, G.711). Dial `1000` from the
softphone to ring line 0 (`1001` for line 1), or dial `555` plus the
softphone's extension on the phone. It has no authentication, so use it on a
trusted network and only while testing. `firmware/bringup/sip_test_call.py`
places a call at the board as a stand-in softphone and reports whether audio
came back.

## Setting up a line by hand

Without the provisioner, the board's web interface generates a ready-to-run
setup script from its own config, and you copy it to the server yourself.

1. **Check availability** on a number to confirm it's free before you commit
   to it (see below for what this check can and can't tell you).
2. Settings → VOIP (turn VoIP on first; its settings are folded away until
   then) → fill in the SIP server and "Line N number (extension)" (and
   password) → **Save**. Leave the number blank to use the line's
   board-to-board number. Takes effect on reboot.
3. Click **Download setup script** — a plain bash script, generated from
   this exact config, that adds this phone's line(s) as SIP peers on the
   Asterisk server without touching any other peers already configured
   there. **Download setup instructions** gives the same steps as a plain
   text file if you'd rather read/print/save them separately.
4. Copy the script to your Asterisk server and run it there, e.g.:
   ```bash
   scp rotary-voip-server-setup.sh user@your-asterisk-host:
   ssh user@your-asterisk-host bash rotary-voip-server-setup.sh
   ```
5. Reboot the phone if you haven't already, then confirm the line shows
   "registered" on the VOIP tab.

The script only ever touches its own marker-tagged blocks (`; BEGIN ...` /
`; END ...` comments in `pjsip.conf` and `extensions.conf`) — safe to
re-run, in any order, any number of times, and safe alongside any other
extensions already on your server. Prefer to add the line by hand instead?
The VOIP tab's "Prefer to add the line by hand instead?" details block
walks through the same `pjsip.conf`/`extensions.conf` edits the script
makes, for editing directly over SSH.

### Checking whether a number is free

Settings → VOIP has a "Check a number" box. It sends a SIP OPTIONS to that
number on your configured server — only a clean `404 Not Found` counts as
"free" and can be trusted. Everything else (including a genuinely free
number) can come back as "in use, or not confirmable" on a PJSIP setup
using `identify_by=username`, since an unrecognized number can get
challenged for auth (`401`) exactly like a real endpoint with the wrong
password. To confirm directly on the server regardless of what the check
said:

```bash
asterisk -rx "pjsip show endpoint <number>"
```

"Unable to find object" means that number is genuinely free.

## Testing with a softphone

A SIP softphone (e.g. Linphone) is a handy second "phone" for testing
without needing a second physical board:

| Field | Value |
|---|---|
| Username | the extension you provisioned |
| Password | the SIP password set when that extension was provisioned |
| Domain | your Asterisk server's hostname or IP — no `sip:` prefix, no port |
| Registrar URI | `sip:<your-asterisk-host>` |
| Outbound SIP Proxy URI | `sip:<your-asterisk-host>:5060;transport=udp` |
| Transport | UDP |

## See also

- [Set up & use](setup.md): the web interface, including Settings → VOIP.
- [`firmware/components/voip/voip.c`](https://github.com/sdylewski/Analog-Redial/blob/main/firmware/components/voip/voip.c):
  the embedded SIP client, if you're debugging registration or call
  problems.
