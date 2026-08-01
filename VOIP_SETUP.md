# VoIP setup — FreePBX on feynman.local, shared with the Pi phones

`feynman.local` runs **FreePBX 17 in Docker** (not plain Asterisk — see
below for why that distinction matters) and hosts phone lines from **both**
projects at once: the ESP32-C6 gateway in this repo, and the older Pi-based
`telephone.py` stack in `pi_rotary_phone`. This doc covers the FreePBX-
specific setup and the shared numbering registry; see
[`pi_rotary_phone/docs/VOIP_SETUP.md`](../pi_rotary_phone/docs/VOIP_SETUP.md)
for baresip/Pi-side detail.

If you're pointing at a *different* server running plain Asterisk (not
FreePBX), skip to ["Plain Asterisk instead of FreePBX"](#plain-asterisk-instead-of-freepbx)
near the bottom — the downloadable-script flow described there is simpler,
but it does **not** work against FreePBX (next section explains why).

## FreePBX vs. plain Asterisk: why this needs a different tool

Plain Asterisk setups can get away with a downloadable script that hand-
edits `pjsip.conf`/`extensions.conf` in marker-tagged blocks (that's what
`pi_rotary_phone/scripts/setup-voip-server.sh` and this firmware's own
"Download Asterisk server setup script" button do). **FreePBX owns those
files** — it regenerates them from its own MySQL database every time you
click "Apply Config" (or run `fwconsole reload`), so hand-edited blocks get
silently discarded. Creating an extension that survives that has to go
through FreePBX's own extension-creation code, not a text edit.

That's what [`voip_provisioner/`](voip_provisioner/) is: a small companion
service that runs on the Docker host (not in any container) and calls
FreePBX's own `Core::processQuickCreate()` — the same call FreePBX's GUI
"Quick Create" form and its GraphQL API both use — when the ESP32's web UI
asks it to. See [`voip_provisioner/README.md`](voip_provisioner/README.md)
for install steps; this doc covers how to use it once it's running.

## RTP audio ports — fix this first

`feynman.local`'s `docker-compose.yaml` only publishes SIP signaling
(5060/udp) and the web UI (8080, 8443) by default. **The RTP media port
range isn't forwarded**, which means calls will connect (you'll hear
ringback, the call will "answer") but carry **no audio** in either
direction. The fix is already built into the compose setup's own `run.sh`
(it configures `iptables` DNAT for the RTP range directly, more efficient
than publishing 16K+ individual Docker ports) — it just needs to actually
be run:

```bash
cd /home/scott/docker/freepbx && sudo ./run.sh
```

Default RTP range is `16384-32767`; pass `--rtp 10000-20000` if you want a
different range (matches Asterisk's own stock default, in case anything
else assumes it). Re-run this after any host reboot — the `iptables` rules
don't persist on their own.

## How a "phone number" works on the ESP32 side

Unlike the Pi (which splits a SIP **username**, e.g. `phone`, from a
separate dialable **extension**, e.g. `1000`), this firmware uses one value
for both — whatever you type into **Settings → VOIP → "Line N
extension/user"** *is* the phone number, and it's what FreePBX's extension
gets created as too. Other phones on the server dial it directly (as `555`
+ that number from another rotary phone, or the number itself from a
softphone).

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
> currently configured with extension `1234` — the same number reserved
> for the echo test (dial `5551234` to hear yourself, per both projects'
> dial guides). Move it to something in the `1100`s (Settings → VOIP →
> Line 1 extension/user, Save, reboot; and use "Auto-provision a line"
> below to create the new number on the FreePBX server, or delete/rename
> the old one via the FreePBX GUI) before both are relied on at once.

## Checking whether a number is free

**Settings → VOIP** has a "Check availability" box. Its behavior depends on
whether a provisioner is configured (same page, "Auto-provisioning"):

- **Provisioner configured (recommended, and what `feynman.local` uses):**
  the check is a direct database lookup on FreePBX via the provisioner —
  authoritative, both "free" and "already in use" can be trusted.
- **No provisioner configured** (plain-Asterisk servers, or before you've
  set one up): falls back to a SIP OPTIONS probe. Read this result
  carefully — it's a real limitation, not a rounding error. Only a clean
  `404 Not Found` counts as "free" and can be trusted; everything else
  (including a genuinely free number) comes back as "in use, or not
  confirmable", because stock Asterisk/PJSIP with `identify_by=username`
  challenges an *unrecognized* number for auth (`401`) exactly the way it
  challenges a *real* endpoint with the wrong password — the two cases are
  indistinguishable over SIP without valid credentials. In testing against
  the real `feynman.local` server (before the provisioner existed), this
  was the answer for every number tried, taken or not.

To confirm directly on the server regardless of which path you used:

```bash
docker exec freepbx-freepbx-1 asterisk -rx "pjsip show endpoint <number>"
```

"Unable to find object" means that number is genuinely free.

## Adding one more ESP32 line (the common case, with the provisioner)

1. On `feynman.local`, confirm `voip_provisioner` is running (see
   [`voip_provisioner/README.md`](voip_provisioner/README.md) if not —
   one-time install).
2. On the new board: **Settings → VOIP → Auto-provisioning** → fill in
   the provisioner's `host:port` (e.g. `feynman.local:8088`) and its setup
   password → Save. Takes effect immediately, no reboot.
3. **Check availability** on a number from the registry above (`1100`,
   `1101`, ...) to confirm it's free.
4. **Auto-provision a line** → same number, a real SIP password, a display
   name → **Create on server**. If the result says "created, but the
   server's config reload failed," that's the known `fwconsole reload`
   issue (see [`voip_provisioner/README.md`](voip_provisioner/README.md))
   — the extension exists in FreePBX's database; you'll need to reload the
   server manually (`docker exec freepbx-freepbx-1 fwconsole reload`, or
   GUI → Apply Config) once, until a Sangoma account gets linked to fix
   `fwconsole reload` generally.
5. Back in the main **Save** form further down the same page: same
   extension as "Line N extension/user", same password, Save, reboot the
   phone.
6. Confirm the line shows "registered" on the VOIP tab after it reconnects.
7. Add a row to the registry table above (in both docs, ideally).

## Dialing between the two projects

Both projects use the same `555` + 4-digit convention for reaching a SIP
extension from a rotary phone, and the same bare-extension dialing from a
softphone. An ESP32 phone at `1100` and a Pi phone at `1000` can call each
other: dial `5551100` from the Pi, or `5551000` from the ESP32 board, or
`1100`/`1000` directly from a softphone registered on the same server.

## Plain Asterisk instead of FreePBX

If you're pointing an ESP32 board at a **different** server running plain
Asterisk (not FreePBX) — this doesn't apply to `feynman.local` any more,
but the older flow still works fine elsewhere:

```bash
# From pi_rotary_phone (installs Asterisk, provisions phone/phone2/mac):
scp scripts/setup-voip-server.sh user@your-server:
ssh user@your-server bash setup-voip-server.sh

# From an ESP32 board's web UI: Settings -> VOIP -> fill in server + a
# number -> Save -> Download server setup script -> scp it over -> run it
# the same way. Only touches this board's own marked block in the config
# files, safe to run alongside the Pi's script or another board's.
```

Both scripts are marker-tagged and idempotent (look for `; BEGIN ...` /
`; END ...` comments in either config file) — safe to re-run, in any order,
any number of times, and safe to mix ESP32 and Pi lines on the same plain-
Asterisk server. None of this applies to FreePBX servers, which is the
whole reason `voip_provisioner/` exists.

## See also

- [`voip_provisioner/README.md`](voip_provisioner/README.md) — install and
  security notes for the companion service this doc assumes is running.
- [`pi_rotary_phone/docs/VOIP_SETUP.md`](../pi_rotary_phone/docs/VOIP_SETUP.md)
  — the Pi side: baresip setup, troubleshooting steps (registration,
  incoming-call event flow, ALSA device quirks), Linphone/softphone
  configuration.
- [`GETTING_STARTED.md`](GETTING_STARTED.md) — the ESP32 web UI walkthrough,
  including the Settings → VOIP tab.
- `firmware/components/voip/voip.c` — the embedded SIP UA implementation
  (scope, what's supported) if you're debugging registration/call issues
  from the ESP32 side specifically.
