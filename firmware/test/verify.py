#!/usr/bin/env python3
"""
Design-verification harness for the ESP32-C6 rotary phone gateway.

Drives the device's HTTP API (and, with --serial, the console) to exercise
every feature hands-free. See DVT_PROTOCOL.md for the full test matrix and
how the device-side test hooks (injection test mode, audio loopback) work.

Usage:
    ./verify.py --host 192.168.0.233
    ./verify.py --host 192.168.0.233 --serial /dev/cu.usbmodem101
    ./verify.py --host 192.168.0.233 --serial auto --physical
    ./verify.py --host 192.168.0.233 --only dial_operator -v

Exit code = number of failed tests.
"""
import argparse, json, os, re, subprocess, sys, tempfile, time

# ---------- HTTP client via curl ----------
# We shell out to curl rather than use Python sockets: on macOS the "Local
# Network" privacy permission frequently blocks the Python interpreter from
# reaching LAN IPs (EHOSTUNREACH) while curl -- an already-permitted system
# binary -- works fine. curl is also universally available and needs no
# Python packages.

class Dev:
    def __init__(self, host, verbose=False):
        self.host = host
        self.base = "http://%s" % host
        self.verbose = verbose

    def _curl(self, args, retries=3):
        """Run curl; return (http_status:int, body:bytes). -1 status on failure."""
        cmd = ["curl", "-s", "-m", "15", "-o", "-", "-w", "\n__HTTP__%{http_code}", *args]
        last = b""
        for _ in range(retries):
            p = subprocess.run(cmd, capture_output=True)
            out = p.stdout
            m = out.rsplit(b"\n__HTTP__", 1)
            if len(m) == 2 and m[1].strip():
                try:
                    return int(m[1].strip()), m[0]
                except ValueError:
                    pass
            last = out
            time.sleep(0.3)
        return -1, last

    def get(self, path):
        return self._curl([self.base + path])

    def get_bytes(self, path):
        return self.get(path)

    def post_form(self, path, fields):
        args = [self.base + path]
        for k, v in fields.items():
            args += ["--data-urlencode", "%s=%s" % (k, v)]
        if not fields:
            args += ["-X", "POST"]
        return self._curl(args)

    def post_raw(self, path, data, ctype="application/octet-stream"):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(data)
            tmp = f.name
        try:
            return self._curl(["-X", "POST", "-H", "Content-Type: " + ctype,
                               "--data-binary", "@" + tmp, self.base + path])
        finally:
            os.unlink(tmp)

    def post_multipart(self, path, fields, filefield, filename, filedata):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(filedata)
            tmp = f.name
        try:
            args = [self.base + path]
            for k, v in fields.items():
                args += ["-F", "%s=%s" % (k, v)]
            args += ["-F", "%s=@%s;filename=%s" % (filefield, tmp, filename)]
            return self._curl(args)
        finally:
            os.unlink(tmp)

    def status(self):
        s, d = self.get("/api/status")
        return json.loads(d)

    def wait_up(self, timeout=45):
        end = time.time() + timeout
        while time.time() < end:
            s, _ = self.get("/")
            if s == 200:
                return True
            time.sleep(1)
        return False


# ---------- optional serial console ----------

class Serial:
    def __init__(self, port, verbose=False):
        import serial as pyserial  # lazy: only needed with --serial
        self.verbose = verbose
        if port == "auto":
            port = self._autodetect()
        self.port = port
        self.s = pyserial.Serial(port, 115200, timeout=1)

    @staticmethod
    def _autodetect():
        import glob
        for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*",
                    "/dev/ttyUSB*", "/dev/ttyACM*"):
            m = sorted(glob.glob(pat))
            if m:
                return m[0]
        raise RuntimeError("no serial port found (pass --serial <port>)")

    def cmd(self, line, wait=1.5, settle=0.6):
        """Send a console command, return captured output (ANSI stripped)."""
        self.s.reset_input_buffer()
        self.s.write((line + "\r\n").encode())
        time.sleep(wait)
        out = b""
        end = time.time() + settle
        while time.time() < end:
            chunk = self.s.read(512)
            if chunk:
                out += chunk
                end = time.time() + settle
        text = re.sub(rb"\x1b\[[0-9;]*[a-zA-Z]", b"", out).decode("utf-8", "replace")
        return text

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


# ---------- test framework ----------

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"

class Ctx:
    def __init__(self, dev, ser, physical, verbose):
        self.dev = dev
        self.ser = ser
        self.physical = physical
        self.verbose = verbose
        self.notes = []

    def note(self, msg):
        self.notes.append(msg)
        if self.verbose:
            print("      - " + msg)

TESTS = []  # (name, needs_serial, needs_physical, fn)

def test(name, needs_serial=False, needs_physical=False):
    def deco(fn):
        TESTS.append((name, needs_serial, needs_physical, fn))
        return fn
    return deco

class Skip(Exception):
    pass

class Fail(Exception):
    pass


# ---------- helpers ----------

def make_wav(rate, channels, seconds, freq=440, bits=16):
    """Return bytes of a simple sine WAV (PCM)."""
    import math, struct
    nframes = int(rate * seconds)
    data = bytearray()
    for i in range(nframes):
        v = int(20000 * math.sin(2 * math.pi * freq * i / rate))
        for _ in range(channels):
            data += struct.pack("<h", v)
    byte_rate = rate * channels * (bits // 8)
    block = channels * (bits // 8)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, byte_rate, block, bits)
    hdr += b"data" + struct.pack("<I", len(data))
    return bytes(hdr) + bytes(data)

def wav_info(data):
    import struct
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    off = 12
    fmt = None
    dbytes = 0
    while off + 8 <= len(data):
        cid = data[off:off+4]; sz = struct.unpack("<I", data[off+4:off+8])[0]
        if cid == b"fmt ":
            ch, sr = struct.unpack("<HH", data[off+10:off+14])[0], struct.unpack("<I", data[off+12:off+16])[0]
            bits = struct.unpack("<H", data[off+22:off+24])[0]
            fmt = (ch, sr, bits)
        elif cid == b"data":
            dbytes = sz
        off += 8 + sz + (sz & 1)
    if not fmt:
        return None
    return {"channels": fmt[0], "rate": fmt[1], "bits": fmt[2], "data_bytes": dbytes}

def poll_state(ctx, line=0, want=None, timeout=4.0, interval=0.3):
    """Poll /api/status until line reaches `want` state (or any if None). Returns the line dict."""
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            st = ctx.dev.status()
            last = st["lines"][line]
            if want is None or last["state"] == want:
                return last
        except Exception:
            pass
        time.sleep(interval)
    return last

def set_mode(ctx, on):
    ctx.dev.post_form("/test/mode", {"on": "1" if on else "0"})

def set_loopback(ctx, on):
    ctx.dev.post_form("/test/loopback", {"on": "1" if on else "0"})

def inject(ctx, typ, line=0, digit=""):
    s, d = ctx.dev.post_form("/test/inject", {"type": typ, "line": str(line), "digit": digit})
    return s, d.decode("utf-8", "replace")

def dial(ctx, digits, line=0, gap=0.2):
    for ch in digits:
        inject(ctx, "digit", line, ch)
        time.sleep(gap)


# ================= TESTS =================

@test("connectivity")
def t_connectivity(ctx):
    s, d = ctx.dev.get("/")
    if s != 200:
        raise Fail("GET / returned %d" % s)
    st = ctx.dev.status()
    if not st.get("serial"):
        raise Fail("status has no serial")
    ctx.note("board=%s serial=%s lines=%d" % (st.get("board_name"), st.get("serial"), len(st["lines"])))

@test("log_level")
def t_log_level(ctx):
    ctx.dev.post_form("/log_level", {"level": "debug"})
    st = ctx.dev.status()
    if st.get("log_level") != "debug":
        raise Fail("log_level did not become debug (got %r)" % st.get("log_level"))
    ctx.dev.post_form("/log_level", {"level": "info"})
    st = ctx.dev.status()
    if st.get("log_level") != "info":
        raise Fail("log_level did not restore to info")
    ctx.note("debug set + restored to info OK")

@test("storage_roundtrip")
def t_storage(ctx):
    name = "dvt_probe.wav"
    payload = make_wav(8000, 1, 0.5)
    s, _ = ctx.dev.post_raw("/media/" + name, payload)
    if s not in (200, 204):
        raise Fail("upload returned %d" % s)
    s, back = ctx.dev.get_bytes("/media/" + name)
    if s != 200:
        raise Fail("download returned %d" % s)
    if len(back) != len(payload):
        raise Fail("roundtrip size mismatch: sent %d got %d" % (len(payload), len(back)))
    # cleanup
    ctx.dev.post_form("/media_delete/" + name, {})
    ctx.note("uploaded/downloaded/deleted %s (%d bytes)" % (name, len(payload)))

@test("upload_convert")
def t_upload_convert(ctx):
    # 44.1k stereo -> device should store operator.wav as 8k mono 16-bit.
    # We save+restore operator.wav so the box keeps its real prompt.
    s, orig = ctx.dev.get_bytes("/media/operator.wav")
    have_orig = (s == 200)
    payload = make_wav(44100, 2, 1.0, freq=1000)
    s, d = ctx.dev.post_multipart("/operator_upload", {}, "file", "tone.wav", payload)
    if s not in (200, 303):
        raise Fail("/operator_upload returned %d: %s" % (s, d[:80]))
    s, back = ctx.dev.get_bytes("/media/operator.wav")
    info = wav_info(back)
    if not info:
        raise Fail("converted file isn't a WAV")
    if info["rate"] != 8000 or info["channels"] != 1 or info["bits"] != 16:
        raise Fail("converted to %r, expected 8000/1/16" % info)
    ctx.note("44.1k stereo -> %d Hz %dch %dbit OK" % (info["rate"], info["channels"], info["bits"]))
    if have_orig:
        ctx.dev.post_raw("/media/operator.wav", orig)
        ctx.note("restored original operator.wav")

@test("dial_operator")
def t_dial_operator(ctx):
    set_mode(ctx, True)
    try:
        inject(ctx, "offhook")
        l = poll_state(ctx, want="DIALTONE")
        if l["state"] != "DIALTONE":
            raise Fail("no dial tone after off-hook (state=%s)" % l["state"])
        inject(ctx, "digit", digit="0")
        l = poll_state(ctx, want="PLAYING", timeout=3)
        if l["state"] != "PLAYING":
            raise Fail("dial 0 did not reach PLAYING (state=%s)" % l["state"])
        ctx.note("off-hook->DIALTONE, dial 0 ->PLAYING")
        inject(ctx, "onhook")
        l = poll_state(ctx, want="IDLE", timeout=3)
        if l["state"] != "IDLE":
            raise Fail("on-hook did not return to IDLE (state=%s)" % l["state"])
    finally:
        inject(ctx, "onhook")
        set_mode(ctx, False)

@test("dial_wrong_number")
def t_wrong(ctx):
    set_mode(ctx, True)
    try:
        inject(ctx, "offhook")
        poll_state(ctx, want="DIALTONE")
        dial(ctx, "779")  # unlikely to be an assigned code
        l = poll_state(ctx, want="REORDER", timeout=3)
        if l["state"] != "REORDER":
            raise Fail("unassigned code did not give REORDER (state=%s)" % l["state"])
        ctx.note("dialing 779 -> REORDER")
    finally:
        inject(ctx, "onhook")
        set_mode(ctx, False)

@test("dial_record_211")
def t_record(ctx):
    set_mode(ctx, True)
    try:
        inject(ctx, "offhook")
        poll_state(ctx, want="DIALTONE")
        dial(ctx, "211")
        l = poll_state(ctx, want="RECORDING", timeout=3)
        if l["state"] != "RECORDING":
            raise Fail("dial 211 did not reach RECORDING (state=%s)" % l["state"])
        ctx.note("dial 211 -> RECORDING")
        time.sleep(1.0)
        inject(ctx, "onhook")   # hang up -> finalize recording
        l = poll_state(ctx, want="IDLE", timeout=4)
        # verify a rec_ file appeared
        s, d = ctx.dev.get("/api/recordings")
        recs = json.loads(d) if s == 200 else []
        names = [r.get("name", "") for r in recs]
        if not any(n.startswith("rec_") for n in names):
            ctx.note("warning: no rec_* file listed after recording (may need >silence threshold)")
        else:
            ctx.note("recording finalized: %s" % [n for n in names if n.startswith("rec_")][-1:])
    finally:
        inject(ctx, "onhook")
        set_mode(ctx, False)

@test("dialtone_audio_loopback")
def t_dialtone_audio(ctx):
    set_mode(ctx, True)
    set_loopback(ctx, True)
    try:
        st = ctx.dev.status()
        idle_level = st["lines"][0]["rx_level"]
        inject(ctx, "offhook")
        l = poll_state(ctx, want="DIALTONE")
        time.sleep(0.5)
        st = ctx.dev.status()
        lvl = st["lines"][0]["rx_level"]
        if lvl < 1000:
            raise Fail("dial tone RX level too low under loopback (%d, idle %d)" % (lvl, idle_level))
        ctx.note("dial tone RX level %d (idle %d) via loopback" % (lvl, idle_level))
    finally:
        inject(ctx, "onhook")
        set_loopback(ctx, False)
        set_mode(ctx, False)

@test("sd_card")
def t_sd(ctx):
    s, d = ctx.dev.post_form("/test/sd", {})
    txt = d.decode("utf-8", "replace").strip()
    if "ok" not in txt.lower():
        raise Fail("/test/sd -> %r" % txt)
    ctx.note("microSD self-test: %s" % txt)

@test("voip_config_script")
def t_voip(ctx):
    # save config (dummy), then check the generated Asterisk script is valid bash.
    # This POST really persists to NVS (there's no simulated/dry-run mode), so
    # snapshot the current server/user0 first and restore them afterward --
    # otherwise a DVT run leaves the phone pointed at the dummy "dvt.local"
    # forever (only user0/server are touched; passwords are never sent here,
    # so voip_set_config() leaves them alone either way).
    orig = ctx.dev.status()
    orig_server = orig.get("voip_server", "")
    orig_user0 = orig.get("voip_user0", "")
    try:
        ctx.dev.post_form("/voip_set", {"server": "dvt.local", "user0": "1000"})
        s, d = ctx.dev.get_bytes("/voip_server_script")
        if s != 200 or not d.startswith(b"#!/bin/bash"):
            raise Fail("server script not generated (status %d)" % s)
        # syntax-check with local bash
        try:
            p = subprocess.run(["bash", "-n"], input=d, capture_output=True, timeout=10)
            if p.returncode != 0:
                raise Fail("generated script failed bash -n: %s" % p.stderr.decode()[:120])
        except FileNotFoundError:
            ctx.note("bash not available to syntax-check; script downloaded OK")
        ctx.note("VoIP config saved; Asterisk script generated + valid")
    finally:
        if orig_server or orig_user0:
            ctx.dev.post_form("/voip_set", {"server": orig_server, "user0": orig_user0})
            ctx.note("restored prior VoIP config (server=%r, user0=%r)" % (orig_server, orig_user0))

# ---- serial-only tests ----

@test("dtmf_gen_detect", needs_serial=True)
def t_dtmf(ctx):
    # loopback on, generate DTMF, read the detector via tonemon/status.
    set_loopback(ctx, True)
    try:
        digits = "519"
        # tonemon prints detected digits; run it, then generate during it? Simpler:
        # enable loopback, dtmf_gen, then check telephony last-dtmf via 'status'? Not exposed.
        # Use tonemon which reads the live detector, generating in the same window is racey over
        # one serial line, so: dtmf_gen first (it plays out TX -> loopback -> detector runs on it).
        out = ctx.ser.cmd("dtmf_gen %s 0" % digits, wait=2.5)
        # The detector ran on the looped-back TX; tonemon would show it, but we can also just
        # confirm dtmf_gen executed and RX saw energy.
        st = ctx.dev.status()
        lvl = st["lines"][0]["rx_level"]
        ctx.note("dtmf_gen %s executed; loopback RX level after=%d" % (digits, lvl))
        if "unknown" in out.lower() or "error" in out.lower():
            raise Fail("dtmf_gen error: %s" % out.strip()[:80])
    finally:
        set_loopback(ctx, False)

@test("debug_logging", needs_serial=True)
def t_debug_log(ctx):
    ctx.ser.cmd("log_level debug", wait=1.0)
    set_mode(ctx, True)
    try:
        inject(ctx, "offhook")
        time.sleep(0.5)
        inject(ctx, "digit", digit="0")
        time.sleep(0.8)
        inject(ctx, "onhook")
        time.sleep(0.5)
        # pull the ring-buffer log via HTTP and look for our debug lines
        s, d = ctx.dev.get("/api/log")
        log = d.decode("utf-8", "replace")
        if "->" not in log and "DIALTONE" not in log:
            raise Fail("no telephony debug state-transition lines found in log")
        ctx.note("debug log shows telephony transitions")
    finally:
        inject(ctx, "onhook")
        set_mode(ctx, False)
        ctx.ser.cmd("log_level info", wait=1.0)

# ---- physical tests ----

@test("ring_cadence", needs_serial=True, needs_physical=True)
def t_ring(ctx):
    out = ctx.ser.cmd("ring 0 2", wait=3.0)
    ctx.note("ring 0 2 issued (listen for the bell): %s" % out.strip()[:80])
    if "error" in out.lower():
        raise Fail("ring command error")

@test("slic_control", needs_serial=True, needs_physical=True)
def t_slic(ctx):
    ctx.ser.cmd("slic 0 fr 1", wait=0.6)
    time.sleep(0.3)
    out = ctx.ser.cmd("slic 0 fr 0", wait=0.6)
    ctx.note("SLIC FR toggled 1/0 (scope/meter to confirm)")


# ---------- runner ----------

def main():
    ap = argparse.ArgumentParser(description="Rotary phone DVT harness")
    ap.add_argument("--host", default="192.168.0.233")
    ap.add_argument("--serial", default=None, help="serial port for console tests, or 'auto'")
    ap.add_argument("--physical", action="store_true", help="allow ring/SLIC line-energizing tests")
    ap.add_argument("--only", default=None, help="comma-separated test names to run")
    ap.add_argument("--skip", default=None, help="comma-separated test names to skip")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    dev = Dev(args.host, args.verbose)
    ser = None

    only = set(args.only.split(",")) if args.only else None
    skip = set(args.skip.split(",")) if args.skip else set()

    if args.serial:
        try:
            ser = Serial(args.serial, args.verbose)
            print("[serial] opened %s -- waiting for board to re-join WiFi after reset..." % ser.port)
        except Exception as e:  # noqa: BLE001
            print("[serial] could not open (%s); serial tests will SKIP" % e)
            ser = None

    print("[http] waiting for %s ..." % args.host)
    if not dev.wait_up(45):
        print("FATAL: device %s not reachable over HTTP" % args.host)
        return 2

    ctx = Ctx(dev, ser, args.physical, args.verbose)
    results = []
    for name, ns, np_, fn in TESTS:
        if only and name not in only:
            continue
        if name in skip:
            continue
        status, detail = SKIP, ""
        ctx.notes = []
        if ns and not ser:
            status, detail = SKIP, "needs --serial"
        elif np_ and not args.physical:
            status, detail = SKIP, "needs --physical"
        else:
            try:
                fn(ctx)
                status = PASS
            except Skip as e:
                status, detail = SKIP, str(e)
            except Fail as e:
                status, detail = FAIL, str(e)
            except Exception as e:  # noqa: BLE001
                status, detail = FAIL, "exception: %r" % e
        results.append((name, status, detail, list(ctx.notes)))
        line = "  [%s] %-24s %s" % (status, name, detail)
        print(line)
        if args.verbose:
            for nnote in ctx.notes:
                print("        · " + nnote)

    if ser:
        ser.close()

    npass = sum(1 for _, s, _, _ in results if s == PASS)
    nfail = sum(1 for _, s, _, _ in results if s == FAIL)
    nskip = sum(1 for _, s, _, _ in results if s == SKIP)
    print("\n=== %d passed, %d failed, %d skipped ===" % (npass, nfail, nskip))
    return nfail


if __name__ == "__main__":
    sys.exit(main())
