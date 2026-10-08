[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md)

# VoIP setup

Analog Redial works completely offline as a two-phone exchange with no
server at all — dialing the other line on the same board just bridges the
two calls locally. This doc covers the **optional** step of registering a
line with a SIP server (Asterisk, FreePBX, or any standards-compliant PBX)
so the board can place and receive calls beyond the phones directly attached
to it.

## How a "phone number" works on the ESP32 side

This firmware uses one value for both the SIP username and the dialable
extension — whatever you type into **Settings → VOIP → "Line N
extension/user"** *is* the phone number, and it's what gets created as the
Asterisk extension too.

## Dialing between lines on a SIP server

The VoIP dial pattern is **`555` + a 4-digit extension**, exactly 7 digits total. It fires the
instant the 7th digit lands, strips the `555` prefix, and dials the
remaining 4 digits. Pick a numbering scheme for your own extensions (e.g.
`1000`, `1001`, `1002`, ...) and keep a note of which numbers are in use on
your server — there's no central registry, your PBX's own config is the
source of truth.

## Setting up a line

There's no companion service and nothing to install on the Asterisk host —
the board's web UI generates a ready-to-run setup script from its own
current config, and you copy it over by hand.

1. **Check availability** on a number to confirm it's free before you commit
   to it (see below for what this check can and can't tell you).
2. Settings → VOIP → fill in the SIP server and "Line N extension/user" (and
   password) → **Save**. Takes effect on reboot.
3. Click **Download setup script** — a plain bash script, generated from
   this exact config, that adds this phone's line(s) as SIP peers on the
   Asterisk server without touching any other peers already configured
   there. **Download setup instructions** gives the same steps as a plain
   text file if you'd rather read/print/save them separately.
4. Copy the script to your Asterisk server and run it there, e.g.:
   ```bash
   scp voip_setup.sh user@your-asterisk-host:
   ssh user@your-asterisk-host bash voip_setup.sh
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
