#!/usr/bin/env python3
"""Analog Redial VoIP provisioner.

A small HTTP service that runs next to Asterisk and lets Analog Redial boards
claim a phone number from their own web UI: check whether a number is free,
register it (the service creates the SIP account and hands back the
credentials), and release it again.

Standard library only. One file holds the server, the admin commands and a
test client:

    redial_provisioner.py serve    --config CONFIG
    redial_provisioner.py add-key  --config CONFIG ID [--min N --max N ...]
    redial_provisioner.py list     --config CONFIG
    redial_provisioner.py release  --config CONFIG NUMBER
    redial_provisioner.py revoke-board --config CONFIG BOARD_ID
    redial_provisioner.py regenerate   --config CONFIG
    redial_provisioner.py client --url URL --key ID.SECRET check|register|release ...

Protocol (version 1), all bodies JSON:

    GET  /api/v1/info                      no auth
    POST /api/v1/check     {number}        key or board auth
    POST /api/v1/register  {board_id, board_name, line, number}
                                           key or board auth
    POST /api/v1/release   {line}          board auth
    POST /api/v1/status    {}              key or board auth

Every POST carries

    Authorization: Redial-HMAC id="key:<key_id>"|"board:<board_id>",
                   ts="<unix seconds>", nonce="<random hex>", sig="<hex>"

where sig = HMAC-SHA256(secret, METHOD "\n" PATH "\n" ts "\n" nonce "\n"
hex(SHA-256(body))). The secret never crosses the network; a timestamp
window plus a nonce cache stops replays. Clients without a set clock take
server_time from /api/v1/info.

Plain HTTP is fine on a LAN. On the internet, put this behind an HTTPS
reverse proxy: the register response carries a SIP password.
"""

import argparse
import contextlib
import fcntl
import hashlib
import hmac
import http.server
import ipaddress
import json
import os
import re
import secrets
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

PROTOCOL_VERSION = 1
CLOCK_SKEW_S = 300
MAX_BODY = 4096
RATE_WINDOW_S = 300
RATE_MAX_REQUESTS = 120
RATE_MAX_AUTH_FAILURES = 10

KEY_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")
BOARD_ID_RE = re.compile(r"^[0-9a-f]{12}$")
NUMBER_RE = re.compile(r"^[1-9][0-9]{1,9}$")
NONCE_RE = re.compile(r"^[0-9a-f]{16,64}$")
AUTH_RE = re.compile(r'(\w+)="([^"]*)"')

DEFAULT_CONFIG = {
    "name": "Analog Redial VoIP",
    "listen": "0.0.0.0",
    "port": 8077,
    "sip_server": "",
    "sip_port": 5060,
    "state_path": "/var/lib/redial-provisioner/state.json",
    "pjsip_path": "/etc/asterisk/redial/pjsip.conf",
    "extensions_path": "/etc/asterisk/redial/extensions.conf",
    "context": "rotary",
    "transport": "",
    "codecs": "ulaw,alaw",
    "permit": [],
    "max_lines_per_board": 2,
    "reload_commands": [["asterisk", "-rx", "pjsip reload"],
                        ["asterisk", "-rx", "dialplan reload"]],
    "exists_command": ["asterisk", "-rx", "pjsip show endpoint {number}"],
    "trust_forwarded_for": False,
    "keys": [],
}


class ApiError(Exception):
    def __init__(self, status, code, message, **extra):
        super().__init__(message)
        self.status, self.code, self.message, self.extra = status, code, message, extra


# ---------------------------------------------------------------- config/state

def load_config(path):
    with open(path) as f:
        cfg = dict(DEFAULT_CONFIG)
        cfg.update(json.load(f))
    if not cfg["sip_server"]:
        raise SystemExit(f"{path}: sip_server is not set")
    for k in cfg["keys"]:
        if not KEY_ID_RE.match(k.get("id", "")) or len(k.get("secret", "")) < 16:
            raise SystemExit(f"{path}: bad key entry {k.get('id')!r}")
    for net in cfg["permit"]:
        ipaddress.ip_network(net, strict=False)
    return cfg


def write_atomic(path, text, mode=0o640):
    """Write via a temp file + rename, keeping the directory owner's uid/gid
    when run as root so admin commands don't leave root-owned files the
    service user can no longer replace."""
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".tmp-")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.chmod(tmp, mode)
        if os.geteuid() == 0:
            st = os.stat(path) if os.path.exists(path) else os.stat(d)
            os.chown(tmp, st.st_uid, st.st_gid)
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(tmp)
        raise


@contextlib.contextmanager
def locked_state(cfg):
    """Load, yield and (if changed) save the state file under an exclusive
    lock shared by the server and the admin commands."""
    path = cfg["state_path"]
    lock_path = path + ".lock"
    if not os.path.exists(lock_path):
        open(lock_path, "a").close()
        if os.geteuid() == 0:
            st = os.stat(os.path.dirname(os.path.abspath(lock_path)))
            os.chown(lock_path, st.st_uid, st.st_gid)
    with open(lock_path, "a") as lockf:
        fcntl.flock(lockf, fcntl.LOCK_EX)
        try:
            state = {"boards": {}, "numbers": {}}
            if os.path.exists(path):
                with open(path) as f:
                    state.update(json.load(f))
            before = json.dumps(state, sort_keys=True)
            yield state
            if json.dumps(state, sort_keys=True) != before:
                write_atomic(path, json.dumps(state, indent=2, sort_keys=True) + "\n", 0o600)
        finally:
            fcntl.flock(lockf, fcntl.LOCK_UN)


# ------------------------------------------------------------- asterisk side

def render_pjsip(cfg, state):
    out = ["; Generated by redial_provisioner.py -- do not edit by hand.",
           "; Changes are made with the provisioner (see its 'list'/'release').", ""]
    for number in sorted(state["numbers"], key=int):
        n = state["numbers"][number]
        board = state["boards"].get(n["board_id"], {})
        label = re.sub(r"[^A-Za-z0-9 _-]", "", board.get("name", "")) or n["board_id"]
        out += [f"[{number}]", "type=aor", "max_contacts=1", "remove_existing=yes", "",
                f"[{number}]", "type=auth", "auth_type=userpass",
                f"username={number}", f"password={n['password']}", "",
                f"[{number}]", "type=endpoint", f"context={cfg['context']}",
                "disallow=all", f"allow={cfg['codecs']}", f"auth={number}", f"aors={number}",
                "identify_by=username",
                "rewrite_contact=yes", "rtp_symmetric=yes", "force_rport=yes",
                "direct_media=no",
                f'callerid="{label} Line {n["line"]}" <{number}>']
        if cfg["transport"]:
            out.append(f"transport={cfg['transport']}")
        if cfg["permit"]:
            out.append("deny=0.0.0.0/0.0.0.0")
            out += [f"permit={net}" for net in cfg["permit"]]
        out.append("")
    return "\n".join(out) + "\n"


def render_extensions(cfg, state):
    # Re-opens the boards' own context rather than defining a separate one
    # to include: Asterisk tries a context's own patterns (a catch-all
    # "_X." is common) before any included context, but an exact number
    # always beats a pattern within the same context.
    out = ["; Generated by redial_provisioner.py -- do not edit by hand.", "",
           f"[{cfg['context']}]"]
    for number in sorted(state["numbers"], key=int):
        out += [f"exten => {number},1,Dial(PJSIP/{number},30)", " same => n,Hangup()"]
    return "\n".join(out) + "\n"


def apply_to_asterisk(cfg, state):
    write_atomic(cfg["pjsip_path"], render_pjsip(cfg, state))
    write_atomic(cfg["extensions_path"], render_extensions(cfg, state))
    for cmd in cfg["reload_commands"]:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        if r.returncode != 0:
            raise ApiError(500, "reload_failed",
                           f"Asterisk reload failed: {' '.join(cmd)}: {r.stderr.strip()[:200]}")


def asterisk_has_endpoint(cfg, number):
    """True if Asterisk already knows an endpoint by this name (for example
    one added by hand or by the older setup script)."""
    cmd = [part.replace("{number}", number) for part in cfg["exists_command"]]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise ApiError(503, "asterisk_unavailable", f"Can't ask Asterisk: {e}")
    text = r.stdout + r.stderr
    if "Unable to find object" in text:
        return False
    if r.returncode != 0:
        raise ApiError(503, "asterisk_unavailable",
                       f"Can't ask Asterisk: {text.strip()[:200]}")
    return True


# ---------------------------------------------------------------- the service

class Service:
    def __init__(self, config_path):
        self.config_path = config_path
        self.cfg = load_config(config_path)
        self.nonces = {}
        self.rate = {}
        self.lock = threading.Lock()

    def reload_config(self):
        self.cfg = load_config(self.config_path)

    # -- abuse limits ------------------------------------------------------

    def rate_check(self, ip, failure=False):
        now = time.time()
        with self.lock:
            if len(self.rate) > 5000:   # forget addresses with nothing recent (internet use)
                self.rate = {k: v for k, v in self.rate.items()
                             if any(now - t < RATE_WINDOW_S for t in v[0] + v[1])}
            reqs, fails = self.rate.get(ip, ([], []))
            reqs = [t for t in reqs if now - t < RATE_WINDOW_S]
            fails = [t for t in fails if now - t < RATE_WINDOW_S]
            if failure:
                fails.append(now)
            else:
                reqs.append(now)
            self.rate[ip] = (reqs, fails)
            if len(fails) > RATE_MAX_AUTH_FAILURES or len(reqs) > RATE_MAX_REQUESTS:
                raise ApiError(429, "rate_limited", "Too many requests; try again in a few minutes")

    # -- authentication ----------------------------------------------------

    def authenticate(self, method, path, headers, body, ip):
        try:
            return self._authenticate(method, path, headers, body)
        except ApiError as e:
            if e.status == 401:
                self.rate_check(ip, failure=True)
            raise

    def _authenticate(self, method, path, headers, body):
        hdr = headers.get("Authorization", "")
        if not hdr.startswith("Redial-HMAC "):
            raise ApiError(401, "auth_required", "Missing Redial-HMAC authorization")
        f = dict(AUTH_RE.findall(hdr))
        ident, ts, nonce, sig = f.get("id", ""), f.get("ts", ""), f.get("nonce", ""), f.get("sig", "")
        if not ts.isdigit() or not NONCE_RE.match(nonce) or not sig:
            raise ApiError(401, "bad_auth", "Malformed authorization")
        now = int(time.time())
        if abs(now - int(ts)) > CLOCK_SKEW_S:
            raise ApiError(401, "stale", "Clock out of range", server_time=now)

        kind, _, name = ident.partition(":")
        if kind == "key":
            entry = next((k for k in self.cfg["keys"] if k["id"] == name), None)
            secret = entry["secret"] if entry else None
        elif kind == "board":
            with locked_state(self.cfg) as st:
                b = st["boards"].get(name)
            secret = b["token"] if b else None
        else:
            secret = None
        if not secret:
            raise ApiError(401, "unknown_id", "Unknown key or board")

        msg = "\n".join([method, path, ts, nonce, hashlib.sha256(body).hexdigest()])
        want = hmac.new(secret.encode(), msg.encode(), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(want, sig.lower()):
            raise ApiError(401, "bad_signature", "Signature does not match")

        with self.lock:
            self.nonces = {n: t for n, t in self.nonces.items() if now - t <= 2 * CLOCK_SKEW_S}
            if nonce in self.nonces:
                raise ApiError(401, "replay", "Request already used")
            self.nonces[nonce] = now
        return kind, name

    # -- helpers -----------------------------------------------------------

    def key_entry(self, key_id):
        return next((k for k in self.cfg["keys"] if k["id"] == key_id), None)

    def check_number(self, number, key_id):
        if not NUMBER_RE.match(number or ""):
            raise ApiError(400, "bad_number", "Numbers are digits only, not starting with 0")
        k = self.key_entry(key_id)
        if not k:
            raise ApiError(403, "key_revoked", "This board's key no longer exists")
        lo, hi = k.get("number_min", 1000), k.get("number_max", 9999)
        if not lo <= int(number) <= hi:
            raise ApiError(400, "out_of_range", f"Pick a number from {lo} to {hi}",
                           number_min=lo, number_max=hi)

    def key_range(self, key_id):
        k = self.key_entry(key_id) or {}
        return {"number_min": k.get("number_min", 1000), "number_max": k.get("number_max", 9999)}

    def board_key_id(self, kind, name, st):
        if kind == "key":
            return name
        return st["boards"][name]["key_id"]

    # -- endpoints ---------------------------------------------------------

    def info(self):
        return {"service": "analog-redial-provisioner", "protocol": PROTOCOL_VERSION,
                "name": self.cfg["name"], "sip_server": self.cfg["sip_server"],
                "sip_port": self.cfg["sip_port"], "server_time": int(time.time())}

    def check(self, auth, body):
        kind, name = auth
        number = str(body.get("number", ""))
        with locked_state(self.cfg) as st:
            key_id = self.board_key_id(kind, name, st)
            self.check_number(number, key_id)
            held = st["numbers"].get(number)
        rng = self.key_range(key_id)
        if held:
            mine = kind == "board" and held["board_id"] == name
            return {"number": number, "available": mine, "reason": "yours" if mine else "taken", **rng}
        if asterisk_has_endpoint(self.cfg, number):
            return {"number": number, "available": False, "reason": "taken", **rng}
        return {"number": number, "available": True, "reason": "free", **rng}

    def register(self, auth, body):
        kind, name = auth
        board_id = str(body.get("board_id", "")).lower()
        number = str(body.get("number", ""))
        line = body.get("line")
        board_name = re.sub(r"[^A-Za-z0-9 _-]", "", str(body.get("board_name", "")))[:32]
        if not NUMBER_RE.match(number):
            raise ApiError(400, "bad_number", "Numbers are digits only, not starting with 0")
        if not BOARD_ID_RE.match(board_id):
            raise ApiError(400, "bad_board_id", "board_id must be 12 hex digits")
        if kind == "board" and board_id != name:
            raise ApiError(403, "wrong_board", "Token belongs to another board")
        if not isinstance(line, int) or not 0 <= line < self.cfg["max_lines_per_board"]:
            raise ApiError(400, "bad_line", "No such line")

        # Asterisk is asked outside the state lock; re-checked under it below.
        with locked_state(self.cfg) as st:
            self.check_number(number, self.board_key_id(kind, name, st))
            already_ours = st["numbers"].get(number, {}).get("board_id") == board_id
        if not already_ours and asterisk_has_endpoint(self.cfg, number):
            raise ApiError(409, "taken", f"{number} is already in use")

        issued_token = None
        with locked_state(self.cfg) as st:
            key_id = self.board_key_id(kind, name, st)
            self.check_number(number, key_id)
            board = st["boards"].get(board_id)
            if kind == "key":
                if board and board["key_id"] != key_id:
                    raise ApiError(409, "board_other_key",
                                   "This board was set up with a different key")
                if not board:
                    k = self.key_entry(key_id)
                    used = sum(1 for b in st["boards"].values() if b["key_id"] == key_id)
                    if used >= k.get("max_boards", 10):
                        raise ApiError(403, "key_full", "This key has no boards left")
                    board = {"key_id": key_id, "created": int(time.time())}
                    st["boards"][board_id] = board
                issued_token = secrets.token_hex(32)
                board["token"] = issued_token
            board["name"] = board_name or board.get("name", "")

            held = st["numbers"].get(number)
            if held and held["board_id"] != board_id:
                raise ApiError(409, "taken", f"{number} is already in use")
            if held and held["line"] != line:
                raise ApiError(409, "other_line", f"{number} is already on your line {held['line']}")
            for n, v in list(st["numbers"].items()):
                if v["board_id"] == board_id and v["line"] == line and n != number:
                    del st["numbers"][n]
            if not held:
                st["numbers"][number] = {"board_id": board_id, "line": line,
                                         "password": secrets.token_urlsafe(18),
                                         "created": int(time.time())}
            password = st["numbers"][number]["password"]
            apply_to_asterisk(self.cfg, st)

        resp = {"number": number, "line": line, "username": number, "password": password,
                "sip_server": self.cfg["sip_server"], "sip_port": self.cfg["sip_port"],
                **self.key_range(key_id)}
        if issued_token:
            resp["board_token"] = issued_token
        return resp

    def release(self, auth, body):
        kind, name = auth
        if kind != "board":
            raise ApiError(403, "board_only", "Release needs the board's own token")
        line = body.get("line")
        with locked_state(self.cfg) as st:
            gone = [n for n, v in st["numbers"].items()
                    if v["board_id"] == name and v["line"] == line]
            for n in gone:
                del st["numbers"][n]
            if gone:
                apply_to_asterisk(self.cfg, st)
        return {"released": gone}

    def status(self, auth, _body):
        """The numbers this key may use, and (for a board) the ones it holds."""
        kind, name = auth
        with locked_state(self.cfg) as st:
            key_id = self.board_key_id(kind, name, st)
            nums = [{"line": v["line"], "number": n}
                    for n, v in st["numbers"].items() if kind == "board" and v["board_id"] == name]
        return {"numbers": sorted(nums, key=lambda x: x["line"]),
                "max_lines": self.cfg["max_lines_per_board"], **self.key_range(key_id)}


def make_handler(svc):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "redial-provisioner"
        sys_version = ""

        def client_ip(self):
            if svc.cfg["trust_forwarded_for"]:
                fwd = self.headers.get("X-Forwarded-For", "")
                if fwd:
                    return fwd.split(",")[0].strip()
            return self.client_address[0]

        def send_json(self, status, obj):
            data = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def handle_api(self, method):
            ip = self.client_ip()
            try:
                svc.reload_config()
                svc.rate_check(ip)
                path = self.path.split("?", 1)[0]
                if method == "GET" and path == "/api/v1/info":
                    return self.send_json(200, svc.info())
                routes = {"/api/v1/check": svc.check, "/api/v1/register": svc.register,
                          "/api/v1/release": svc.release, "/api/v1/status": svc.status}
                if method != "POST" or path not in routes:
                    raise ApiError(404, "not_found", "No such endpoint")
                length = int(self.headers.get("Content-Length") or 0)
                if length > MAX_BODY:
                    raise ApiError(413, "too_large", "Request too large")
                body = self.rfile.read(length)
                auth = svc.authenticate(method, path, self.headers, body, ip)
                try:
                    parsed = json.loads(body or b"{}")
                    if not isinstance(parsed, dict):
                        raise ValueError
                except ValueError:
                    raise ApiError(400, "bad_json", "Body must be a JSON object")
                self.send_json(200, routes[path](auth, parsed))
            except ApiError as e:
                self.send_json(e.status, {"error": e.code, "message": e.message, **e.extra})
            except Exception as e:  # keep the service up; details stay in the log
                self.log_error("internal error: %r", e)
                self.send_json(500, {"error": "internal", "message": "Internal error"})

        def do_GET(self):
            self.handle_api("GET")

        def do_POST(self):
            self.handle_api("POST")

        def log_message(self, fmt, *args):
            sys.stderr.write("%s %s\n" % (self.client_ip(), fmt % args))

    return Handler


def serve(args):
    svc = Service(args.config)
    with locked_state(svc.cfg) as st:
        apply_to_asterisk(svc.cfg, st)
    srv = http.server.ThreadingHTTPServer((svc.cfg["listen"], svc.cfg["port"]), make_handler(svc))
    print(f"redial-provisioner listening on {svc.cfg['listen']}:{svc.cfg['port']}", flush=True)
    srv.serve_forever()


# ------------------------------------------------------------- admin commands

def cmd_add_key(args):
    if not KEY_ID_RE.match(args.id):
        raise SystemExit("key id: lowercase letters, digits and dashes, up to 32")
    with open(args.config) as f:
        raw = json.load(f)
    if any(k["id"] == args.id for k in raw.get("keys", [])):
        raise SystemExit(f"key {args.id!r} already exists")
    secret = secrets.token_urlsafe(24)
    raw.setdefault("keys", []).append({
        "id": args.id, "secret": secret, "label": args.label or args.id,
        "number_min": args.min, "number_max": args.max, "max_boards": args.max_boards})
    write_atomic(args.config, json.dumps(raw, indent=2) + "\n", 0o640)
    print(f"{args.id}.{secret}")


def cmd_list(args):
    cfg = load_config(args.config)
    with locked_state(cfg) as st:
        for k in cfg["keys"]:
            boards = {b: v for b, v in st["boards"].items() if v["key_id"] == k["id"]}
            print(f"key {k['id']} ({k.get('label', '')}): numbers "
                  f"{k.get('number_min', 1000)}-{k.get('number_max', 9999)}, "
                  f"{len(boards)}/{k.get('max_boards', 10)} boards")
            for b, v in sorted(boards.items()):
                nums = sorted((n["line"], num) for num, n in st["numbers"].items() if n["board_id"] == b)
                lines = ", ".join(f"line {ln}: {num}" for ln, num in nums) or "no numbers"
                print(f"  board {b} {v.get('name', '')!r}: {lines}")


def cmd_release(args):
    cfg = load_config(args.config)
    with locked_state(cfg) as st:
        if st["numbers"].pop(args.number, None) is None:
            raise SystemExit(f"{args.number} isn't registered")
        apply_to_asterisk(cfg, st)
    print(f"released {args.number}")


def cmd_revoke_board(args):
    cfg = load_config(args.config)
    with locked_state(cfg) as st:
        if st["boards"].pop(args.board_id, None) is None:
            raise SystemExit(f"no board {args.board_id}")
        for n in [n for n, v in st["numbers"].items() if v["board_id"] == args.board_id]:
            del st["numbers"][n]
        apply_to_asterisk(cfg, st)
    print(f"revoked {args.board_id}")


def cmd_regenerate(args):
    cfg = load_config(args.config)
    with locked_state(cfg) as st:
        apply_to_asterisk(cfg, st)
    print("regenerated and reloaded")


# ----------------------------------------------------------------- test client

def signed_post(url, path, ident, secret, body, offset=0):
    data = json.dumps(body).encode()
    ts = str(int(time.time()) + offset)
    nonce = secrets.token_hex(16)
    msg = "\n".join(["POST", path, ts, nonce, hashlib.sha256(data).hexdigest()])
    sig = hmac.new(secret.encode(), msg.encode(), hashlib.sha256).hexdigest()
    req = urllib.request.Request(url.rstrip("/") + path, data=data, method="POST", headers={
        "Content-Type": "application/json",
        "Authorization": f'Redial-HMAC id="{ident}", ts="{ts}", nonce="{nonce}", sig="{sig}"'})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)


def cmd_client(args):
    with urllib.request.urlopen(args.url.rstrip("/") + "/api/v1/info", timeout=10) as r:
        info = json.load(r)
    offset = info["server_time"] - int(time.time())
    if args.board_token:
        board, _, secret = args.board_token.partition(":")
        ident = f"board:{board}"
    else:
        kid, _, secret = args.key.partition(".")
        ident = f"key:{kid}"
    if args.action == "info":
        status, out = 200, info
    elif args.action == "check":
        status, out = signed_post(args.url, "/api/v1/check", ident, secret, {"number": args.number}, offset)
    elif args.action == "register":
        status, out = signed_post(args.url, "/api/v1/register", ident, secret,
                                  {"board_id": args.board_id, "board_name": args.board_name,
                                   "line": args.line, "number": args.number}, offset)
    elif args.action == "release":
        status, out = signed_post(args.url, "/api/v1/release", ident, secret, {"line": args.line}, offset)
    else:
        status, out = signed_post(args.url, "/api/v1/status", ident, secret, {}, offset)
    print(status, json.dumps(out, indent=2))
    return 0 if status == 200 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Analog Redial VoIP provisioner")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("serve", "list", "regenerate"):
        p = sub.add_parser(name)
        p.add_argument("--config", required=True)
    p = sub.add_parser("add-key", help="create an enrollment key and print it")
    p.add_argument("--config", required=True)
    p.add_argument("id")
    p.add_argument("--label")
    p.add_argument("--min", type=int, default=1000)
    p.add_argument("--max", type=int, default=1999)
    p.add_argument("--max-boards", type=int, default=10)
    p = sub.add_parser("release", help="free a number")
    p.add_argument("--config", required=True)
    p.add_argument("number")
    p = sub.add_parser("revoke-board", help="forget a board and free its numbers")
    p.add_argument("--config", required=True)
    p.add_argument("board_id")
    p = sub.add_parser("client", help="talk to a running provisioner (testing)")
    p.add_argument("--url", required=True)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--key", help="ID.SECRET enrollment key")
    g.add_argument("--board-token", help="BOARD_ID:TOKEN")
    p.add_argument("action", choices=("info", "check", "register", "release", "status"))
    p.add_argument("number", nargs="?", default="")
    p.add_argument("--board-id", default="0123456789ab")
    p.add_argument("--board-name", default="test")
    p.add_argument("--line", type=int, default=0)
    args = ap.parse_args(argv)
    handlers = {"serve": serve, "add-key": cmd_add_key, "list": cmd_list, "release": cmd_release,
                "revoke-board": cmd_revoke_board, "regenerate": cmd_regenerate, "client": cmd_client}
    return handlers[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main() or 0)
