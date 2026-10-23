#!/usr/bin/env python3
"""Tests for redial_provisioner.py, run against a fake `asterisk` command.

    python3 -m unittest test_redial_provisioner.py
"""

import hashlib
import hmac
import http.server
import json
import os
import secrets
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import redial_provisioner as rp  # noqa: E402

FAKE_ASTERISK = """
import sys
existing = open(sys.argv[1]).read().split()
print("Endpoint: " + sys.argv[2] if sys.argv[2] in existing else "Unable to find object " + sys.argv[2])
"""

BOARD_A = "aaaaaaaaaaaa"
BOARD_B = "bbbbbbbbbbbb"


class ProvisionerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        fake = os.path.join(d, "fake_asterisk.py")
        with open(fake, "w") as f:
            f.write(FAKE_ASTERISK)
        self.existing = os.path.join(d, "existing.txt")
        with open(self.existing, "w") as f:
            f.write("101 102\n")
        self.cfg_path = os.path.join(d, "config.json")
        self.secret = secrets.token_urlsafe(24)
        self.other_secret = secrets.token_urlsafe(24)
        cfg = {
            "sip_server": "pbx.example", "port": 0,
            "state_path": os.path.join(d, "state.json"),
            "pjsip_path": os.path.join(d, "pjsip_redial.conf"),
            "extensions_path": os.path.join(d, "extensions_redial.conf"),
            "permit": ["192.168.0.0/24"],
            "reload_commands": [[sys.executable, "-c", "pass"]],
            "exists_command": [sys.executable, fake, self.existing, "{number}"],
            "context": "internal",
            "keys": [
                {"id": "home", "secret": self.secret, "number_min": 1000, "number_max": 1999, "max_boards": 2},
                {"id": "friend", "secret": self.other_secret, "number_min": 2000, "number_max": 2099},
            ],
        }
        with open(self.cfg_path, "w") as f:
            json.dump(cfg, f)
        self.svc = rp.Service(self.cfg_path)
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), rp.make_handler(self.svc))
        self.url = f"http://127.0.0.1:{self.srv.server_address[1]}"
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.tmp.cleanup()

    # -- helpers --

    def post(self, path, body, ident="key:home", secret=None, ts=None, nonce=None, sig=None):
        secret = self.secret if secret is None else secret
        data = json.dumps(body).encode()
        ts = str(int(time.time())) if ts is None else str(ts)
        nonce = nonce or secrets.token_hex(16)
        msg = "\n".join(["POST", path, ts, nonce, hashlib.sha256(data).hexdigest()])
        sig = sig or hmac.new(secret.encode(), msg.encode(), hashlib.sha256).hexdigest()
        req = urllib.request.Request(self.url + path, data=data, method="POST", headers={
            "Authorization": f'Redial-HMAC id="{ident}", ts="{ts}", nonce="{nonce}", sig="{sig}"'})
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.load(r)
        except urllib.error.HTTPError as e:
            return e.code, json.load(e)

    def post_raw(self, path, body):
        """Key-signed POST that returns (status, content type, raw bytes)."""
        data = json.dumps(body).encode()
        ts, nonce = str(int(time.time())), secrets.token_hex(16)
        msg = "\n".join(["POST", path, ts, nonce, hashlib.sha256(data).hexdigest()])
        sig = hmac.new(self.secret.encode(), msg.encode(), hashlib.sha256).hexdigest()
        req = urllib.request.Request(self.url + path, data=data, method="POST", headers={
            "Authorization": f'Redial-HMAC id="key:home", ts="{ts}", nonce="{nonce}", sig="{sig}"'})
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, r.headers["Content-Type"], r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers["Content-Type"], e.read()

    def set_cfg(self, **changes):
        with open(self.cfg_path) as f:
            cfg = json.load(f)
        cfg.update(changes)
        with open(self.cfg_path, "w") as f:
            json.dump(cfg, f)

    def fake_tts(self):
        """A stand-in for redial_tts.py: echoes the text inside a fake WAV."""
        seen = []

        class H(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                seen.append(body["text"])
                if body["text"] == "fail":
                    data, ctype, code = b'{"error":"bad_text","message":"no audio"}', "application/json", 400
                else:
                    data, ctype, code = b"RIFF" + body["text"].encode(), "audio/wav", 200
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("X-Redial-Cache", "hit" if body["text"].startswith("cached") else "miss")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                data = b'{"ok": true, "voice": "af_sarah"}'
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        srv = http.server.HTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        self.set_cfg(tts_url=f"http://127.0.0.1:{srv.server_address[1]}")
        return seen

    def register(self, number, board=BOARD_A, line=0, **kw):
        return self.post("/api/v1/register",
                         {"board_id": board, "board_name": "kitchen", "line": line, "number": number}, **kw)

    def read(self, name):
        with open(os.path.join(self.tmp.name, name)) as f:
            return f.read()

    # -- tests --

    def test_tts_off_by_default(self):
        status, _, body = self.post_raw("/api/v1/tts", {"text": "hello"})
        self.assertEqual((status, json.loads(body)["error"]), (503, "tts_unavailable"))
        with urllib.request.urlopen(self.url + "/api/v1/info") as r:
            self.assertFalse(json.load(r)["tts"])

    def test_tts_returns_audio_and_enforces_limits(self):
        seen = self.fake_tts()
        status, ctype, body = self.post_raw("/api/v1/tts", {"text": "  Leave a   message. "})
        self.assertEqual((status, ctype, body), (200, "audio/wav", b"RIFFLeave a message."))
        self.assertEqual(seen, ["Leave a message."])
        self.assertTrue(self.post("/api/v1/status", {})[1]["tts"])

        status, _, body = self.post_raw("/api/v1/tts", {"text": "x" * 501})
        self.assertEqual((status, json.loads(body)["error"]), (400, "bad_text"))
        status, _, body = self.post_raw("/api/v1/tts", {"text": "fail"})
        self.assertEqual((status, json.loads(body)["message"]), (400, "no audio"))

        self.set_cfg(tts_per_hour=3)   # 2 counted so far (the over-long text was refused first)
        self.assertEqual(self.post_raw("/api/v1/tts", {"text": "one more"})[0], 200)
        status, _, body = self.post_raw("/api/v1/tts", {"text": "too many"})
        self.assertEqual((status, json.loads(body)["error"]), (429, "tts_quota"))

    def test_tts_cache_hits_are_free_and_voice_is_reported(self):
        self.fake_tts()
        with urllib.request.urlopen(self.url + "/api/v1/info") as r:
            self.assertEqual(json.load(r)["tts_voice"], "af_sarah")
        self.set_cfg(tts_per_hour=1)
        for _ in range(3):
            self.assertEqual(self.post_raw("/api/v1/tts", {"text": "cached digit"})[0], 200)
        self.assertEqual(self.post_raw("/api/v1/tts", {"text": "fresh"})[0], 200)
        self.assertEqual(self.post_raw("/api/v1/tts", {"text": "fresh again"})[0], 429)

    def test_tts_board_token_and_revoked_key(self):
        self.fake_tts()
        token = self.register("1005")[1]["board_token"]
        status, _ = self.post("/api/v1/status", {}, ident=f"board:{BOARD_A}", secret=token)
        self.assertEqual(status, 200)
        self.set_cfg(keys=[{"id": "friend", "secret": self.other_secret}])
        data = json.dumps({"text": "hi"}).encode()
        ts, nonce = str(int(time.time())), secrets.token_hex(16)
        msg = "\n".join(["POST", "/api/v1/tts", ts, nonce, hashlib.sha256(data).hexdigest()])
        sig = hmac.new(token.encode(), msg.encode(), hashlib.sha256).hexdigest()
        req = urllib.request.Request(self.url + "/api/v1/tts", data=data, method="POST", headers={
            "Authorization": f'Redial-HMAC id="board:{BOARD_A}", ts="{ts}", nonce="{nonce}", sig="{sig}"'})
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req)
        self.assertEqual(cm.exception.code, 403)

    def test_info_needs_no_auth(self):
        with urllib.request.urlopen(self.url + "/api/v1/info") as r:
            info = json.load(r)
        self.assertEqual(info["sip_server"], "pbx.example")
        self.assertLess(abs(info["server_time"] - time.time()), 5)

    def test_check_free_taken_and_range(self):
        self.assertEqual(self.post("/api/v1/check", {"number": "1005"})[1]["reason"], "free")
        with open(self.existing, "a") as f:
            f.write("1006\n")
        self.assertEqual(self.post("/api/v1/check", {"number": "1006"})[1]["reason"], "taken")
        status, out = self.post("/api/v1/check", {"number": "2005"})
        self.assertEqual((status, out["error"]), (400, "out_of_range"))
        status, out = self.post("/api/v1/check", {"number": "10; reboot"})
        self.assertEqual((status, out["error"]), (400, "bad_number"))

    def test_register_issues_token_and_writes_asterisk_config(self):
        status, out = self.register("1005")
        self.assertEqual(status, 200)
        self.assertEqual(out["username"], "1005")
        self.assertEqual(out["sip_server"], "pbx.example")
        self.assertIn("board_token", out)
        pj = self.read("pjsip_redial.conf")
        self.assertIn("[1005]\ntype=endpoint", pj)
        self.assertIn(f"password={out['password']}", pj)
        self.assertIn('callerid="kitchen Line 0" <1005>', pj)
        self.assertIn("deny=0.0.0.0/0.0.0.0\npermit=192.168.0.0/24", pj)
        ext = self.read("extensions_redial.conf")
        self.assertIn("[internal]\nexten => 1005,1,Dial(PJSIP/1005,30)", ext)
        self.assertIn("context=internal", pj)
        self.assertNotIn("transport=", pj)
        self.assertEqual(os.stat(os.path.join(self.tmp.name, "state.json")).st_mode & 0o777, 0o600)

        tok = out["board_token"]
        status, again = self.register("1005", ident=f"board:{BOARD_A}", secret=tok)
        self.assertEqual(status, 200)
        self.assertEqual(again["password"], out["password"])
        self.assertNotIn("board_token", again)
        status, st = self.post("/api/v1/status", {}, ident=f"board:{BOARD_A}", secret=tok)
        self.assertEqual(st["numbers"], [{"line": 0, "number": "1005"}])
        self.assertEqual((st["number_min"], st["number_max"]), (1000, 1999))
        status, st = self.post("/api/v1/status", {}, ident="key:friend", secret=self.other_secret)
        self.assertEqual((status, st["number_min"], st["number_max"], st["numbers"]), (200, 2000, 2099, []))
        self.assertEqual(self.post("/api/v1/check", {"number": "1005"},
                                   ident=f"board:{BOARD_A}", secret=tok)[1]["reason"], "yours")

    def test_number_belongs_to_one_board(self):
        self.register("1005")
        status, out = self.register("1005", board=BOARD_B)
        self.assertEqual((status, out["error"]), (409, "taken"))
        status, out = self.register("101")
        self.assertEqual((status, out["error"]), (400, "out_of_range"))

    def test_changing_a_lines_number_releases_the_old_one(self):
        tok = self.register("1005")[1]["board_token"]
        self.register("1007", ident=f"board:{BOARD_A}", secret=tok)
        pj = self.read("pjsip_redial.conf")
        self.assertIn("[1007]", pj)
        self.assertNotIn("[1005]", pj)
        status, out = self.register("1007", line=1, ident=f"board:{BOARD_A}", secret=tok)
        self.assertEqual((status, out["error"]), (409, "other_line"))

    def test_release_needs_board_token(self):
        tok = self.register("1005")[1]["board_token"]
        status, out = self.post("/api/v1/release", {"line": 0})
        self.assertEqual((status, out["error"]), (403, "board_only"))
        status, out = self.post("/api/v1/release", {"line": 0}, ident=f"board:{BOARD_A}", secret=tok)
        self.assertEqual(out["released"], ["1005"])
        self.assertNotIn("[1005]", self.read("pjsip_redial.conf"))

    def test_board_cannot_switch_keys_and_tokens_are_board_bound(self):
        tok = self.register("1005")[1]["board_token"]
        status, out = self.register("2005", ident="key:friend", secret=self.other_secret)
        self.assertEqual((status, out["error"]), (409, "board_other_key"))
        status, out = self.register("1006", board=BOARD_B, ident=f"board:{BOARD_A}", secret=tok)
        self.assertEqual((status, out["error"]), (403, "wrong_board"))

    def test_key_board_limit(self):
        self.register("1005", board="000000000001")
        self.register("1006", board="000000000002")
        status, out = self.register("1007", board="000000000003")
        self.assertEqual((status, out["error"]), (403, "key_full"))

    def test_auth_failures(self):
        status, out = self.post("/api/v1/check", {"number": "1005"}, sig="00" * 32)
        self.assertEqual((status, out["error"]), (401, "bad_signature"))
        status, out = self.post("/api/v1/check", {"number": "1005"}, secret="wrong-secret-xxxxxx")
        self.assertEqual(out["error"], "bad_signature")
        status, out = self.post("/api/v1/check", {"number": "1005"}, ident="key:nobody")
        self.assertEqual(out["error"], "unknown_id")
        status, out = self.post("/api/v1/check", {"number": "1005"}, ts=int(time.time()) - 3600)
        self.assertEqual(out["error"], "stale")
        self.assertIn("server_time", out)
        nonce = secrets.token_hex(16)
        self.assertEqual(self.post("/api/v1/check", {"number": "1005"}, nonce=nonce)[0], 200)
        status, out = self.post("/api/v1/check", {"number": "1005"}, nonce=nonce)
        self.assertEqual(out["error"], "replay")

    def test_repeated_auth_failures_are_rate_limited(self):
        for _ in range(rp.RATE_MAX_AUTH_FAILURES):
            self.post("/api/v1/check", {"number": "1005"}, sig="00" * 32)
        status, out = self.post("/api/v1/check", {"number": "1005"}, sig="00" * 32)
        self.assertEqual((status, out["error"]), (429, "rate_limited"))

    def test_admin_commands_share_state_with_the_server(self):
        self.register("1005")
        rp.main(["release", "--config", self.cfg_path, "1005"])
        self.assertNotIn("[1005]", self.read("pjsip_redial.conf"))
        self.assertEqual(self.post("/api/v1/check", {"number": "1005"})[1]["reason"], "free")
        rp.main(["add-key", "--config", self.cfg_path, "newbie", "--min", "3000", "--max", "3099"])
        with open(self.cfg_path) as f:
            new = next(k for k in json.load(f)["keys"] if k["id"] == "newbie")
        status, _ = self.register("3001", board=BOARD_B, ident="key:newbie", secret=new["secret"])
        self.assertEqual(status, 200)


if __name__ == "__main__":
    unittest.main()
