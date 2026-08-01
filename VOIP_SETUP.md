# VoIP setup — native Asterisk on feynman.local

`feynman.local` runs **native Asterisk 22** (`asterisk.service`, installed
via package manager — no Docker, no FreePBX) and hosts phone lines from
**both** projects at once: the ESP32-C6 gateway in this repo, and the older
Pi-based `telephone.py` stack in `pi_rotary_phone`. This doc covers the
setup and the shared numbering registry; see
[`pi_rotary_phone/docs/VOIP_SETUP.md`](../pi_rotary_phone/docs/VOIP_SETUP.md)
for baresip/Pi-side detail.

**History, if you're catching up:** this server ran FreePBX in Docker for
a while (see the `archive/freepbx-voip-provisioner` git tag and this
file's own git history for that setup) — it was retired in favor of native
Asterisk, which needs none of FreePBX's OAuth2/GraphQL/module-dependency
machinery to automate. Before FreePBX, `feynman.local` ran plain Asterisk
too, with a `[rotary]` dialplan context — that config was **also** retired
when the personal/staff phone system (below) was set up fresh. If you find
old docs/scripts referencing FreePBX or a `[rotary]` context on this
server, they're stale.

## This server also runs a separate, non-rotary-phone phone system

`feynman.local` hosts **extensions 101-104**, a personal/staff softphone
system (TLS on 5061, voicemail, a public hostname via DDNS) set up and
maintained separately — see `/home/scott/asterisk-config/CLAUDE.md` on the
server itself for its own notes. **Not related to this project.** Don't
touch these extensions or the `[internal]` context's existing entries when
working on rotary-phone lines — `voip_provisioner` (below) is specifically
built to never go near them; it only ever adds/removes its own
marker-tagged blocks (`; BEGIN <extension>` / `; END <extension>`).

## How a "phone number" works on the ESP32 side

Unlike the Pi (which splits a SIP **username**, e.g. `phone`, from a
separate dialable **extension**, e.g. `1000`), this firmware uses one value
for both — whatever you type into **Settings → VOIP → "Line N
extension/user"** *is* the phone number, and it's what gets created as the
Asterisk extension too.

## Extension registry (update this by hand)

There's no central authority handing out numbers — this table **is** the
source of truth. Before picking a number for a new phone: check it isn't
already listed here, use the availability check below as a second opinion,
then **add a row here** once you've committed to it.

| Extension | Device | Purpose | Notes |
|-----------|--------|---------|-------|
| `101`-`104` | Personal/staff softphones | Not this project | Don't touch — see above |
| `105`+ | *(reserved block for rotary phones — Pi and ESP32 both)* | this project | — |

**Recommendation:** number rotary phones sequentially from `105` — no need
for the old 4-digit `1000`/`1100` split between Pi and ESP32 boards now
that both retired configs are gone; there's no ambiguity to avoid on a
fresh registry. A 2-line board uses two consecutive numbers.

> **As of this writing:** board "MaxPhone" needs to be (re-)provisioned
> here — its old FreePBX-era extension (`1234`) no longer exists on this
> server at all. Give it `105` (see "Adding one more line" below).

## Checking whether a number is free

**Settings → VOIP** has a "Check availability" box. Its behavior depends on
whether a provisioner is configured (same page, "Auto-provisioning"):

- **Provisioner configured (recommended):** authoritative — the
  provisioner checks the actual `pjsip.conf` on the server for a
  `[<number>]` section, so both "free" and "already in use" can be
  trusted, regardless of whether that section was created by this tool or
  by hand (e.g. it correctly reports `101`-`104` as in use even though
  `voip_provisioner` never touched them).
- **No provisioner configured:** falls back to a SIP OPTIONS probe. Read
  this result carefully — it's a real limitation, not a rounding error.
  Only a clean `404 Not Found` counts as "free" and can be trusted;
  everything else (including a genuinely free number) can come back as
  "in use, or not confirmable" on a PJSIP setup using
  `identify_by=username`, since an unrecognized number can get challenged
  for auth (`401`) exactly like a real endpoint with the wrong password —
  the two cases aren't always distinguishable without valid credentials.

To confirm directly on the server regardless of which path you used:

```bash
asterisk -rx "pjsip show endpoint <number>"
```

"Unable to find object" means that number is genuinely free.

## Adding one more line (the common case, with the provisioner)

1. On `feynman.local`, confirm `voip_provisioner` is running (see
   [`voip_provisioner/README.md`](voip_provisioner/README.md) if not —
   one-time install, needs `sudo` for the systemd service).
2. On the new board: **Settings → VOIP → Auto-provisioning** → fill in
   the provisioner's `host:port` (e.g. `feynman.local:8088`) and its setup
   password → Save. Takes effect immediately, no reboot.
3. **Check availability** on a number from the registry above (`105`,
   `106`, ...) to confirm it's free.
4. **Auto-provision a line** → same number, a real SIP password, a display
   name → **Create on server**. Unlike the old FreePBX setup, there's no
   separate "reload" step that can fail for unrelated reasons — native
   Asterisk's `pjsip reload`/`dialplan reload` just works.
5. Back in the main **Save** form further down the same page: same
   extension as "Line N extension/user", same password, Save, reboot the
   phone.
6. Confirm the line shows "registered" on the VOIP tab after it reconnects.
7. Add a row to the registry table above (in both docs, ideally).

No RTP port-forwarding concerns here (unlike the old Dockerized FreePBX
setup) — native Asterisk on the host doesn't sit behind Docker's NAT, and
rotary phones are LAN-only anyway (no router port-forward needed, that's
only a concern for the separate personal/staff extensions' remote access).

## No provisioner? Manual / scripted fallback

Same marker-tagged, idempotent technique `voip_provisioner` itself uses,
available as a standalone script if you'd rather not run a service:

```bash
# From pi_rotary_phone (installs Asterisk if needed, provisions
# phone/phone2/mac -- override PHONE_EXT etc. for different numbers):
scp scripts/setup-voip-server.sh user@your-server:
ssh user@your-server bash setup-voip-server.sh

# From an ESP32 board's web UI: Settings -> VOIP -> fill in server + a
# number -> Save -> Download server setup script -> scp it over -> run it
# the same way.
```

Both scripts (and `voip_provisioner`) only ever touch their own
marker-tagged blocks (`; BEGIN ...` / `; END ...` comments in either config
file) — safe to re-run, in any order, any number of times, and safe to mix
ESP32 and Pi lines (or the separate personal/staff extensions) on the same
server.

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
