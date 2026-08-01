# voip_provisioner — ESP32-driven Asterisk extension creation

Lets an ESP32 rotary phone's own web UI (Settings → VOIP) create its
Asterisk extension directly, instead of you hand-editing config files over
SSH for every new phone. See [`../VOIP_SETUP.md`](../VOIP_SETUP.md) for the
full picture (numbering, manual-fallback instructions) — this file is just
the install steps for this one piece.

**This targets native Asterisk** (a real `asterisk.service`, no Docker, no
FreePBX) — the setup this project moved to on `feynman.local` after
retiring FreePBX. If you're still on the FreePBX-based version, see the
`archive/freepbx-voip-provisioner` git tag for that implementation (OAuth2/
GraphQL + `docker exec`); this file replaced it because native Asterisk
needs none of that — just two text files and `asterisk -rx`.

## What's here

- `provisioner.py` — the whole thing. Runs as the `asterisk` system user
  (needs to, to write `/etc/asterisk/*.conf` and reach the reload socket).
  Stdlib only, no dependencies. Writes marker-tagged blocks (`; BEGIN
  <extension>` / `; END <extension>`) into `pjsip.conf` (a new AOR/auth/
  endpoint trio) and into the existing `[internal]` dialplan context in
  `extensions.conf` (Dial + Hangup, no voicemail — this is a hardware
  phone, not a person with a mailbox) — same "only touch your own marked
  block" idempotency this project uses everywhere it generates Asterisk
  config. Never touches other extensions (e.g. `101`-`104`, personal/staff
  softphones) or any other config.
- `rotary-voip-provisioner.service` — systemd unit template.

## Install (on feynman.local, or wherever Asterisk actually runs)

Deployed under `/opt`, not a home directory — the `asterisk` user can't
traverse a typical `750` home directory at all, which fails with a
confusing `200/CHDIR` (or a silent permission-denied reading the password
file) even if you `chown` the individual files correctly.

```bash
# 1. Copy this directory to the Asterisk host, then move it into /opt
scp -r voip_provisioner/ scott@feynman.local:/tmp/voip_provisioner
ssh scott@feynman.local
sudo mv /tmp/voip_provisioner /opt/rotary-voip-provisioner
cd /opt/rotary-voip-provisioner

# 2. Pick a real setup password and lock the whole directory down to the
#    asterisk user (the service runs as) -- not scott, not "other".
sudo python3 -c "import secrets; print(secrets.token_urlsafe(18))" | sudo tee setup_password.txt >/dev/null
sudo chown -R asterisk:asterisk /opt/rotary-voip-provisioner
sudo chmod 700 /opt/rotary-voip-provisioner
sudo chmod 600 setup_password.txt

# 3. Install + start the service
sudo cp rotary-voip-provisioner.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rotary-voip-provisioner

# 4. Confirm it's up
systemctl status rotary-voip-provisioner --no-pager
curl -s "http://localhost:8088/check?extension=105&password=$(sudo cat setup_password.txt)"
```

Then in each ESP32 phone's Settings → VOIP page: fill in "Provisioner
host:port" (`feynman.local:8088`) and "Setup password" (the contents of
`setup_password.txt`) alongside the usual server/extension/password
fields — see `VOIP_SETUP.md`. Nothing on the firmware side changed from
the FreePBX-era version — same wire protocol.

## What it does NOT do

- Doesn't touch anything except the one extension you ask it to create —
  `101`-`104` (or any other existing extension) are never read or written.
- Doesn't create a voicemail box — this is provisioning a hardware phone,
  not a person.
- Doesn't need Docker, PHP, OAuth2, or GraphQL — direct file edits +
  `asterisk -rx "pjsip reload"` / `"dialplan reload"`, which (unlike
  FreePBX's `fwconsole reload`) has no module-dependency chain to break.

## Security notes

- LAN bring-up tool, matching this project's existing posture elsewhere
  (see `esp32_rotary_phone/firmware/components/webui/webui.c`): one shared
  password, no TLS, no per-caller identity. **Don't port-forward 8088 on
  your router.** (This is unrelated to whatever port-forwards you've set
  up for the personal/staff Asterisk extensions on TCP 5061 — those are a
  separate concern for a separate purpose.)
- The password gates *creating extensions on your PBX* — pick something
  real, not `changeme`. `setup_password.txt` is `.gitignore`'d; never
  commit a real one.
- Runs as the `asterisk` system user — the same account the Asterisk
  process itself runs as, so this doesn't grant any *additional*
  privilege beyond what Asterisk already has. Still, it's a network-
  reachable service that can rewrite live PBX config given the right
  password, so treat the password with real care.

## Uninstall

```bash
sudo systemctl disable --now rotary-voip-provisioner
sudo rm /etc/systemd/system/rotary-voip-provisioner.service
sudo systemctl daemon-reload
```
