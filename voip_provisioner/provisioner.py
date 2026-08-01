#!/usr/bin/env python3
"""
provisioner.py -- small password-gated HTTP service that lets an ESP32
rotary phone's web UI create its own Asterisk extension directly, instead
of hand-editing config files over SSH for every new phone.

Run this ON THE ASTERISK HOST (feynman.local), as the `asterisk` system
user -- that's the owner of /etc/asterisk/{pjsip,extensions}.conf (mode
644, owner-write-only) and the only account that can reach the
/var/run/asterisk/asterisk.ctl control socket for `asterisk -rx` reloads.
See rotary-voip-provisioner.service.

This replaces an earlier FreePBX-based version of this file (see the
archive/freepbx-voip-provisioner git tag) -- FreePBX was retired on
feynman.local in favor of native Asterisk, which needs none of that
version's OAuth2/GraphQL/Docker machinery: just two text files and
`asterisk -rx`. Deliberately kept the *same* wire protocol (POST
/provision, POST /check, identical request/response JSON shapes) so the
ESP32 firmware needed zero changes for this swap.

Writes marker-tagged blocks (`; BEGIN <extension>` / `; END <extension>`)
into pjsip.conf (a new AOR/auth/endpoint trio) and into the existing
[internal] dialplan context in extensions.conf (Dial + Hangup, no
voicemail -- this is a hardware phone, not a person with a mailbox to
check) -- exact same "only touch your own marked block" idempotency this
project has used everywhere else it generates Asterisk config
(pi_rotary_phone/scripts/setup-voip-server.sh, the ESP32's own downloadable
script generator). Existing extensions (101-104, personal/staff
softphones) are never touched.

Endpoints (all LAN-plain-HTTP, JSON in/out):
    POST /provision   {"password": "...", "extension": "105",
                        "secret": "...", "name": "MaxPhone line 1"}
                       -> {"ok": bool, "message": str, "reload_ok": bool}
    POST /check       {"password": "...", "extension": "105"}
                       -> {"ok": bool, "in_use": bool}
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from hmac import compare_digest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

EXT_RE = re.compile(r"^[0-9]{2,6}$")
PJSIP_CONF = Path("/etc/asterisk/pjsip.conf")
EXTENSIONS_CONF = Path("/etc/asterisk/extensions.conf")
DIALPLAN_CONTEXT = "internal"  # matches the existing 101-104 context


def marker_present(path: Path, extension: str) -> bool:
    text = path.read_text(encoding="utf-8")
    return re.search(rf"^; BEGIN {re.escape(extension)}$", text, re.MULTILINE) is not None


def section_exists(extension: str) -> bool:
    """True if `[<extension>]` appears as a section header in pjsip.conf,
    regardless of whether *this* tool created it. This is the real
    existence check used before creating anything -- checking only for our
    own `; BEGIN` marker would report an extension like 101-104 (created
    by hand, not by this tool) as "free", and creating one anyway would
    silently duplicate the section header and corrupt an already-live
    extension. marker_present() (marker-only) is kept separately for the
    narrower "did *this tool* put this here" question `remove_marker_block`
    and re-provisioning logic need."""
    text = PJSIP_CONF.read_text(encoding="utf-8")
    return re.search(rf"^\[{re.escape(extension)}\]", text, re.MULTILINE) is not None


def remove_marker_block(text: str, extension: str) -> str:
    pattern = rf"^; BEGIN {re.escape(extension)}$.*?^; END {re.escape(extension)}$\n?"
    return re.sub(pattern, "", text, flags=re.MULTILINE | re.DOTALL)


def add_pjsip_endpoint(extension: str, secret: str, name: str) -> None:
    text = PJSIP_CONF.read_text(encoding="utf-8")
    # create_extension() already rejects an existing section before calling
    # this, so this is normally a no-op -- kept as a defensive no-duplicate
    # guard in case that gate is ever bypassed (e.g. a future retry path).
    text = remove_marker_block(text, extension)
    block = f"""
; ── Extension {extension} ({name}) ── rotary phone, added by voip_provisioner ──
; BEGIN {extension}
[{extension}]
type=aor
max_contacts=1
remove_existing=yes

[{extension}]
type=auth
auth_type=userpass
username={extension}
password={secret}

[{extension}]
type=endpoint
context={DIALPLAN_CONTEXT}
disallow=all
allow=ulaw,alaw,opus,g722
auth={extension}
aors={extension}
identify_by=username
; END {extension}
"""
    PJSIP_CONF.write_text(text.rstrip("\n") + "\n" + block, encoding="utf-8")


def add_dialplan_entry(extension: str) -> None:
    text = EXTENSIONS_CONF.read_text(encoding="utf-8")
    text = remove_marker_block(text, extension)
    block = (
        f"; BEGIN {extension}\n"
        f"exten => {extension},1,Dial(PJSIP/{extension},20)\n"
        f" same => n,Hangup()\n"
        f"; END {extension}\n"
    )
    # Insert right after the "[internal]" context header, same
    # insert-after-header technique used elsewhere in this project, so the
    # entry lands inside that context rather than at end-of-file (which
    # would silently fall into [default] instead).
    header = f"[{DIALPLAN_CONTEXT}]"
    idx = text.find(header)
    if idx == -1:
        raise RuntimeError(f'"{header}" context not found in {EXTENSIONS_CONF}')
    insert_at = text.index("\n", idx) + 1
    new_text = text[:insert_at] + block + text[insert_at:]
    EXTENSIONS_CONF.write_text(new_text, encoding="utf-8")


def remove_extension(extension: str) -> None:
    """Cleanup helper for a failed create -- only ever removes *our own*
    marker-tagged blocks (remove_marker_block), never touches a bare
    [<extension>] section this tool didn't create."""
    for path in (PJSIP_CONF, EXTENSIONS_CONF):
        text = path.read_text(encoding="utf-8")
        text = remove_marker_block(text, extension)
        path.write_text(text, encoding="utf-8")


def reload_asterisk() -> bool:
    ok = True
    for cmd in ("pjsip reload", "dialplan reload"):
        p = subprocess.run(["asterisk", "-rx", cmd], capture_output=True, text=True, timeout=15)
        if p.returncode != 0:
            ok = False
    return ok


def create_extension(extension: str, secret: str, name: str) -> dict:
    if section_exists(extension):
        return {"ok": False, "message": f"extension {extension} already exists"}
    try:
        add_pjsip_endpoint(extension, secret, name)
        add_dialplan_entry(extension)
    except Exception as e:  # noqa: BLE001 -- report to caller, don't leave a half-written config
        remove_extension(extension)  # best-effort cleanup
        return {"ok": False, "message": f"exception: {e}"}
    reload_ok = reload_asterisk()
    msg = f"extension {extension} created"
    if not reload_ok:
        msg += " (created, but 'asterisk -rx pjsip/dialplan reload' failed -- check the server)"
    return {"ok": True, "message": msg, "reload_ok": reload_ok}


def check_extension(extension: str) -> dict:
    return {"ok": True, "in_use": section_exists(extension)}


class Handler(BaseHTTPRequestHandler):
    password: str = ""

    def _send_json(self, status: int, body: dict) -> None:
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _check_password(self, supplied: str) -> bool:
        return compare_digest(supplied or "", self.password)

    def _read_json_body(self) -> dict | None:
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0 or length > 4096:
            self._send_json(400, {"ok": False, "message": "bad request body"})
            return None
        try:
            return json.loads(self.rfile.read(length))
        except json.JSONDecodeError:
            self._send_json(400, {"ok": False, "message": "invalid JSON"})
            return None

    def do_POST(self) -> None:
        if self.path == "/provision":
            body = self._read_json_body()
            if body is None:
                return
            if not self._check_password(body.get("password", "")):
                self._send_json(403, {"ok": False, "message": "bad setup password"})
                return
            extension = str(body.get("extension", ""))
            secret = str(body.get("secret", ""))
            name = str(body.get("name", ""))[:64]
            if not EXT_RE.match(extension):
                self._send_json(400, {"ok": False, "message": "extension must be 2-6 digits"})
                return
            if not (6 <= len(secret) <= 64):
                self._send_json(400, {"ok": False, "message": "secret must be 6-64 characters"})
                return
            if not name:
                self._send_json(400, {"ok": False, "message": "name is required"})
                return
            result = create_extension(extension, secret, name)
            self._send_json(200 if result.get("ok") else 409, result)
            return

        if self.path == "/check":
            body = self._read_json_body()
            if body is None:
                return
            if not self._check_password(body.get("password", "")):
                self._send_json(403, {"ok": False, "message": "bad setup password"})
                return
            extension = str(body.get("extension", ""))
            if not EXT_RE.match(extension):
                self._send_json(400, {"ok": False, "message": "extension must be 2-6 digits"})
                return
            self._send_json(200, check_extension(extension))
            return

        self._send_json(404, {"ok": False, "message": "not found"})

    def do_GET(self) -> None:
        # Query-string form of /check, kept for quick curl/browser testing --
        # the ESP32 firmware always uses the POST form above.
        if not self.path.startswith("/check"):
            self._send_json(404, {"ok": False, "message": "not found"})
            return
        from urllib.parse import parse_qs, urlparse
        q = parse_qs(urlparse(self.path).query)
        supplied = (q.get("password") or [""])[0]
        extension = (q.get("extension") or [""])[0]
        if not self._check_password(supplied):
            self._send_json(403, {"ok": False, "message": "bad setup password"})
            return
        if not EXT_RE.match(extension):
            self._send_json(400, {"ok": False, "message": "extension must be 2-6 digits"})
            return
        self._send_json(200, check_extension(extension))

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--password-file", required=True, help="File containing the shared setup password (chmod 600)")
    p.add_argument("--port", type=int, default=8088)
    p.add_argument("--bind", default="0.0.0.0", help="LAN-only, please -- don't port-forward this")
    args = p.parse_args()

    pw_path = Path(args.password_file)
    password = pw_path.read_text(encoding="utf-8").strip()
    if not password:
        raise SystemExit(f"{pw_path} is empty -- put a real password in it")
    mode = pw_path.stat().st_mode & 0o777
    if mode & 0o077:
        print(f"warning: {pw_path} is readable by others (mode {oct(mode)}) -- "
              f"chmod 600 {pw_path}", file=sys.stderr)

    if not PJSIP_CONF.exists() or not EXTENSIONS_CONF.exists():
        raise SystemExit(f"{PJSIP_CONF} or {EXTENSIONS_CONF} not found -- is Asterisk installed?")

    Handler.password = password

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"listening on {args.bind}:{args.port}, provisioning into {PJSIP_CONF}/{EXTENSIONS_CONF}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
