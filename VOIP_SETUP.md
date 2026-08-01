# VoIP setup — shared Asterisk server for ESP32 and Pi phones

One Asterisk server (a Raspberry Pi, a spare machine, whatever — this doc
calls it `feynman.local` throughout, matching [`pi_rotary_phone`'s own
setup](../pi_rotary_phone/docs/VOIP_SETUP.md)) can host phone lines from
**both** projects at once: the ESP32-C6 gateway in this repo, and the older
Pi-based `telephone.py` stack in `pi_rotary_phone`. Both sides now generate
Asterisk config the same safe way, so you can mix them freely.

## The one thing that used to bite people

Older versions of `pi_rotary_phone/scripts/setup-voip-server.sh` **rewrote
`pjsip.conf` and `extensions.conf` from scratch on every run** — fine the
first time, but running it again (say, to rotate a password) silently wiped
out any phone line added another way, including an ESP32 board's own
generated script. That's fixed: both scripts now only touch their own
marked block (look for `; BEGIN ...` / `; END ...` comments in either config
file) and leave everything else alone. Re-running either one, in any order,
any number of times, is safe. If you're on an old checkout of
`pi_rotary_phone`, update it before you add an ESP32 phone to a server it
already manages.

## How a "phone number" works here

Unlike the Pi (which splits a SIP **username**, e.g. `phone`, from a
separate dialable **extension**, e.g. `1000`), this firmware uses one value
for both — whatever you type into **Settings → VOIP → "Line N
extension/user"** *is* the phone number. Other phones on the server dial it
directly (as `555` + that number from another rotary phone, or the number
itself from a softphone); Asterisk also authenticates the line's SIP
registration under that same name. There's no separate "set the phone
number" step — that field *is* the phone number.

## Extension registry (update this by hand)

There's no central authority handing out numbers — this table **is** the
source of truth, same as the Pi's own doc already was for its 4 numbers.
Before picking a number for a new phone: check it isn't already listed
here, then use the availability check below as a second opinion, then
**add a row here** once you've committed to it (in both this file and the
Pi's, if either doc is the one your team actually reads before adding a
phone).

| Extension | Device | Project | SIP user |
|-----------|--------|---------|----------|
| `1000` | Rotary phone line 1 | Pi (`pi_rotary_phone`) | `phone` |
| `1001` | Rotary phone line 2 | Pi (`pi_rotary_phone`) | `phone2` |
| `2000` | Mac softphone | Pi (`pi_rotary_phone`) | `mac` / `softphone` |
| `1234` | Echo test (built into the server setup script) | shared | — |
| `1100`+ | *(reserved block for ESP32 boards — see below)* | this repo | — |

**Recommendation:** give each ESP32 board a number in the `1100`–`1199`
range (`1100`, `1101`, ...) — a 2-line board uses two consecutive numbers.
That keeps them visually distinct from the Pi's `1000`s at a glance and
leaves `1002`–`1099` free if the Pi setup ever grows more lines. Nothing in
the firmware enforces this — it's a convention, not a limit — so it only
works if this table stays current.

> **Known conflict as of this writing:** a board named "MaxPhone" is
> currently configured with extension `1234` — the same number the shared
> server setup script reserves for the echo test (dial `5551234` to hear
> yourself, per both projects' dial guides). Move it to something in the
> `1100`s (Settings → VOIP → Line 1 extension/user, Save, reboot, then
> re-download and re-run that board's setup script) before both are relied
> on at once.

## Checking whether a number is free

**Settings → VOIP** has a "Check availability" box: it sends a SIP OPTIONS
to a candidate number on the server and reports back. Read the result
correctly — this is a real limitation, not a rounding error:

- **"looks free"** means the server answered `404 Not Found` — nothing by
  that name exists. This is the one answer you can trust.
- **"in use, or not confirmable"** covers everything else, including a
  number that's genuinely free. Stock Asterisk/PJSIP with
  `identify_by=username` (what both projects' setup scripts configure)
  challenges an *unrecognized* number for authentication (`401`) exactly
  the way it challenges a *real* endpoint with the wrong password — the
  two cases are indistinguishable over SIP without credentials for every
  possibility. In testing against a real server, this was the answer for
  every number tried, taken or not.

So: a "free" result is reliable. Anything else is inconclusive, not a
confirmed collision. To actually confirm, log into the Asterisk box and run:

```bash
sudo asterisk -rx "pjsip show endpoint <number>"
```

No output (or "Unable to find") means that number is genuinely free. This,
plus keeping the registry table above current, is the real source of truth
— the in-UI check is a convenience first pass, not a substitute for it.

## First-time server setup

Either project's script bootstraps the shared infrastructure (Asterisk
install, `[transport-udp]`, the `[rotary]` dialplan context, the `1234` echo
test, the `_X.` catch-all) if it isn't there yet, then provisions its own
account(s). Run **one** of these — it doesn't matter which:

```bash
# From pi_rotary_phone (installs Asterisk, provisions phone/phone2/mac):
scp scripts/setup-voip-server.sh user@feynman.local:
ssh user@feynman.local bash setup-voip-server.sh

# From an ESP32 board's web UI (installs Asterisk if needed, provisions
# just that board's line(s)): Settings -> VOIP -> fill in server + a number
# from the registry above -> Save -> Download server setup script -> scp it
# over and run it the same way.
```

## Adding one more line (the common case)

This is the "just add a phone" path — no full server reinstall.

**Adding another ESP32 board:**
1. Pick a free number from the registry above (check availability first).
2. On the new board: Settings → VOIP → SIP server = `feynman.local`
   (or your server's name/IP), Line 1 extension/user = the number you
   picked, set a password, Save, reboot.
3. Settings → VOIP → "Download Asterisk server setup script" → copy it to
   the server → run it there. It only touches this board's own marked
   block.
4. Back on the board's Settings → VOIP page, confirm the line shows
   "registered" after it reconnects.
5. Add a row to the registry table above (in both docs, ideally).

**Adding another Pi line / softphone:** see
[`pi_rotary_phone/docs/VOIP_SETUP.md`](../pi_rotary_phone/docs/VOIP_SETUP.md)
— same idea, `setup-voip-server.sh` with the relevant `*_EXT`/`*_USER`/
`*_PASS` environment variables overridden for the new account, re-run on
the server. It won't touch any ESP32 lines already there.

## Dialing between the two projects

Both projects use the same `555` + 4-digit convention for reaching a SIP
extension from a rotary phone, and the same bare-extension dialing from a
softphone. An ESP32 phone at `1100` and a Pi phone at `1000` can call each
other: dial `5551100` from the Pi, or `5551000` from the ESP32 board, or
`1100`/`1000` directly from a softphone registered on the same server.

## See also

- [`pi_rotary_phone/docs/VOIP_SETUP.md`](../pi_rotary_phone/docs/VOIP_SETUP.md)
  — the Pi side: baresip setup, troubleshooting steps (registration,
  incoming-call event flow, ALSA device quirks), Linphone/softphone
  configuration. Still the deeper reference for anything Pi-specific;
  this doc only covers where the two projects need to agree.
- [`GETTING_STARTED.md`](GETTING_STARTED.md) — the ESP32 web UI walkthrough,
  including the Settings → VOIP tab.
- `firmware/components/voip/voip.c` — the embedded SIP UA implementation
  (scope, what's supported) if you're debugging registration/call issues
  from the ESP32 side specifically.
