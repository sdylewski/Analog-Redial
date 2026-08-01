#!/usr/bin/env python3
"""
provisioner.py -- small password-gated HTTP service that lets an ESP32
rotary phone's web UI create its own FreePBX extension, instead of you
clicking through the FreePBX GUI by hand.

Run this ON THE DOCKER HOST (feynman.local), NOT inside the freepbx
container -- it shells out to `docker cp`/`docker exec` to run
add_extension.php (see that file for why: FreePBX's own
Core::processQuickCreate() is the only way to create an extension that
survives FreePBX's next config regeneration, and that class only exists
inside the container's PHP autoloader).

Security model: this is a LAN bring-up tool, matching the rest of this
project's posture (see esp32_rotary_phone's webui.c) -- one shared setup
password, no TLS, no per-user accounts. Don't port-forward this port on
your router. The password file should be chmod 600, owned by whoever runs
this service.

Usage:
    ./provisioner.py --password-file ./setup_password.txt \\
        --container freepbx-freepbx-1 --port 8088

Endpoints (all LAN-plain-HTTP, JSON in/out):
    POST /provision   {"password": "...", "extension": "1101",
                        "secret": "...", "name": "MaxPhone line 1"}
                       -> {"ok": bool, "message": str, "reload_ok": bool}
    GET  /check?extension=1101&password=...
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
HELPER_NAME = "add_extension.php"
HELPER_CONTAINER_PATH = "/tmp/add_extension.php"


def docker_cp_helper(container: str) -> None:
    """Copies add_extension.php into the container. Cheap and idempotent --
    called before every /provision request so a container recreate (image
    update, etc.) never leaves this stale."""
    local = Path(__file__).parent / HELPER_NAME
    subprocess.run(
        ["docker", "cp", str(local), f"{container}:{HELPER_CONTAINER_PATH}"],
        check=True, capture_output=True, timeout=15,
    )


def create_extension(container: str, extension: str, secret: str, name: str) -> dict:
    docker_cp_helper(container)
    p = subprocess.run(
        ["docker", "exec", container, "php", HELPER_CONTAINER_PATH, extension, secret, name],
        capture_output=True, text=True, timeout=30,
    )
    try:
        result = json.loads(p.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return {"ok": False, "message": f"unexpected helper output: {p.stdout!r} {p.stderr!r}"}

    if not result.get("ok"):
        return result

    # Best-effort: FreePBX's own config regen (fwconsole reload / GUI Apply
    # Config) can fail on a fresh install with an unmet "sipsettings" ->
    # "backup" module dependency -- fixed by installing both directly (see
    # README.md's install step 3; NOT a Sangoma-account issue, despite the
    # error message). The extension is correctly in FreePBX's database
    # either way; only making it *live* on the wire needs this to succeed.
    reload_p = subprocess.run(
        ["docker", "exec", container, "fwconsole", "reload"],
        capture_output=True, text=True, timeout=60,
    )
    result["reload_ok"] = reload_p.returncode == 0
    if not result["reload_ok"]:
        result["message"] += (
            " (created, but the server's config reload failed -- run "
            "'fwconsole reload' or GUI Apply Config on the server to make "
            "it live; see VOIP_SETUP.md)"
        )
    return result


def check_extension(container: str, extension: str) -> dict:
    docker_cp_helper(container)
    p = subprocess.run(
        ["docker", "exec", container, "php", HELPER_CONTAINER_PATH, extension, "--check-only"],
        capture_output=True, text=True, timeout=30,
    )
    try:
        result = json.loads(p.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return {"ok": False, "in_use": None, "message": "check failed"}
    # --check-only's "ok" IS "is free" -- see add_extension.php.
    return {"ok": True, "in_use": not result.get("ok", False)}


class Handler(BaseHTTPRequestHandler):
    container: str = ""
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
        # Both endpoints take a JSON body (not query-string args, even for
        # /check) so the ESP32 side never has to URL-encode a password --
        # see esp32_rotary_phone/firmware/components/webui/webui.c.
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
            result = create_extension(self.container, extension, secret, name)
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
            self._send_json(200, check_extension(self.container, extension))
            return

        self._send_json(404, {"ok": False, "message": "not found"})

    def do_GET(self) -> None:
        # Query-string form of /check, kept for quick curl/browser testing --
        # the ESP32 firmware always uses the POST form above.
        if not self.path.startswith("/check"):
            self._send_json(404, {"ok": False, "message": "not found"})
            return
        from urllib.parse import urlparse, parse_qs
        q = parse_qs(urlparse(self.path).query)
        supplied = (q.get("password") or [""])[0]
        extension = (q.get("extension") or [""])[0]
        if not self._check_password(supplied):
            self._send_json(403, {"ok": False, "message": "bad setup password"})
            return
        if not EXT_RE.match(extension):
            self._send_json(400, {"ok": False, "message": "extension must be 2-6 digits"})
            return
        self._send_json(200, check_extension(self.container, extension))

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--password-file", required=True, help="File containing the shared setup password (chmod 600)")
    p.add_argument("--container", default="freepbx-freepbx-1", help="FreePBX container name (docker ps)")
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

    Handler.container = args.container
    Handler.password = password

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"listening on {args.bind}:{args.port}, provisioning into container '{args.container}'")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
