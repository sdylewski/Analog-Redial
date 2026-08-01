# voip_provisioner — ESP32-driven FreePBX extension creation

Lets an ESP32 rotary phone's own web UI (Settings → VOIP) create its
FreePBX extension directly, instead of you clicking through the FreePBX
GUI by hand for every new phone. See
[`../VOIP_SETUP.md`](../VOIP_SETUP.md) for the full picture (numbering
registry, manual-fallback instructions, the RTP port gotcha) — this file
is just the install steps for this one piece.

## Why this exists instead of a downloadable script

Plain Asterisk setups (see `pi_rotary_phone`) can get away with a
downloadable bash script that hand-edits `pjsip.conf`/`extensions.conf` in
marker-tagged blocks. **FreePBX owns those files** — it regenerates them
from its own database on every "Apply Config," so hand edits don't stick.
The only way to create an extension that survives that is to go through
FreePBX's own extension-creation code
(`FreePBX\modules\Core::processQuickCreate()`, the same call its GraphQL
API and GUI "Quick Create" form use), which only exists inside the
container's PHP autoloader — so this has to run *in* the container, and
something has to be listening for the ESP32's request to trigger it.

## What's here

- `add_extension.php` — runs *inside* the freepbx container. Creates one
  PJSIP extension (no voicemail, no user-manager account — just a phone),
  or reports it already exists. `--check-only` mode reports existence
  without creating anything.
- `provisioner.py` — runs on the **Docker host** (not in any container).
  A small password-gated HTTP service (stdlib only, no dependencies) that
  `docker cp`'s the PHP helper into the container and `docker exec`'s it.
- `rotary-voip-provisioner.service` — systemd unit template.

## Install (on feynman.local, or wherever the FreePBX container runs)

```bash
# 1. Copy this directory to the Docker host, next to docker-compose.yaml
scp -r voip_provisioner/ scott@feynman.local:/home/scott/docker/freepbx/

# 2. Pick a real setup password (this gates who can create extensions --
#    treat it like any other admin credential) and lock the file down
ssh scott@feynman.local
cd /home/scott/docker/freepbx/voip_provisioner
python3 -c "import secrets; print(secrets.token_urlsafe(18))" > setup_password.txt
chmod 600 setup_password.txt

# 3. One-time fix on a fresh FreePBX install: `fwconsole reload` (which this
#    service calls after every create) needs the "sipsettings" module,
#    which itself needs "backup" -- neither ships by default. Install both
#    directly (NOT `fwconsole ma installlocal`, which has a broken
#    dependency-resolution path and will report a confusing "Cant Reach
#    Online Server"/"Retrieved Module XML Was Empty" even though the repo
#    is reachable fine -- no Sangoma account needed, that's a red herring):
docker exec freepbx-freepbx-1 fwconsole ma downloadinstall backup sipsettings
docker exec freepbx-freepbx-1 fwconsole reload   # should print "Reload Complete"

# 4. Install + start the service
sudo cp rotary-voip-provisioner.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rotary-voip-provisioner

# 5. Confirm it's up
systemctl status rotary-voip-provisioner --no-pager
curl -s http://localhost:8088/check?extension=1101\&password=$(cat setup_password.txt)
```

Then in each ESP32 phone's Settings → VOIP page: fill in "Provisioner
host:port" (`feynman.local:8088`) and "Setup password" (the contents of
`setup_password.txt`) alongside the usual server/extension/password
fields — see `VOIP_SETUP.md`.

## What it does NOT do

- Doesn't touch anything except the one extension you ask it to create —
  every other endpoint, trunk, and setting on your FreePBX box is
  untouched.
- Doesn't create a voicemail box or a User Manager / UCP login for the
  extension — this is provisioning a hardware phone, not a person.
- Doesn't reach the FreePBX GraphQL API or need the "api" module at all —
  it calls the same underlying `Core::processQuickCreate()` FreePBX's own
  GUI and API both call.
- **Doesn't guarantee the extension is live on the wire** if the server's
  own `fwconsole reload` is broken for unrelated reasons (see step 3 of
  Install above — a fresh install is commonly missing the `sipsettings`/
  `backup` modules, not a Sangoma-account issue). If `reload_ok` comes back
  `false` in the response, the extension exists in FreePBX's database but
  isn't live yet; fix the underlying `fwconsole reload` failure on the
  server and re-run it (`docker exec freepbx-freepbx-1 fwconsole reload`) —
  no need to recreate the extension.

## Security notes

- LAN bring-up tool, matching this project's existing posture elsewhere
  (see `esp32_rotary_phone/firmware/components/webui/webui.c`): one shared
  password, no TLS, no per-caller identity. **Don't port-forward 8088 on
  your router.**
- The password gates *creating extensions on your PBX* — pick something
  real, not `changeme`. `setup_password.txt` is `.gitignore`'d; never
  commit a real one.
- Runs as the `scott` user with `docker exec`/`docker cp` access to the
  freepbx container, which is effectively root-equivalent for that
  container's contents. Don't run this as a more privileged user than you
  have to.

## Uninstall

```bash
sudo systemctl disable --now rotary-voip-provisioner
sudo rm /etc/systemd/system/rotary-voip-provisioner.service
sudo systemctl daemon-reload
```
