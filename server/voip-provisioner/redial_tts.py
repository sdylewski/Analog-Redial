#!/usr/bin/env python3
"""Analog Redial text-to-speech service.

Renders spoken prompts for boards, in one server-wide voice (Kokoro, default
af_sarah), as the WAV a board plays best: 8 kHz mono 8-bit mu-law, trimmed
of leading and trailing silence, peak just under full scale.

It listens on 127.0.0.1 only. Boards never talk to it directly: the
provisioner (redial_provisioner.py) authenticates them, applies the quota and
forwards POST /api/v1/tts here. Runs in its own virtual environment
(kokoro-onnx, numpy, soundfile), installed by `install.sh --with-tts`.

    redial_tts.py serve  --models DIR [--voice af_sarah] [--port 8078] [--cache DIR]
    redial_tts.py render --models DIR "Some text" -o out.wav

API:
    GET  /health                      {"ok": true, "voice": ...}
    POST /synthesize  {"text": "..."} audio/wav, or a JSON error

Single-threaded on purpose: one render at a time, the rest wait their turn.
Results are cached by voice and text, so the same prompt again costs nothing;
a cached answer carries `X-Redial-Cache: hit` so the provisioner doesn't count it.

Every WAV carries a tag in a LIST/INFO "ICMT" chunk (players skip it):

    redial-tts voice=<voice> text=<first 16 hex digits of SHA-256(text)>

where text has its whitespace collapsed to single spaces. A board reads it to
tell whether a file already matches the voice and text it wants.
"""

import argparse
import hashlib
import http.server
import json
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np

OUT_RATE = 8000
PEAK = 10 ** (-0.5 / 20)        # -0.5 dBFS
SILENCE = 10 ** (-45 / 20)      # trim threshold
MAX_TEXT = 500
CACHE_MAX_FILES = 500
MODEL_FILES = ("kokoro-v1.0.onnx", "voices-v1.0.bin")


# -- audio post-processing (pure numpy; tested without the model) -------------

def lowpass(x, rate, cutoff=3600.0, taps=101):
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff / rate * n) * np.hamming(taps)
    return np.convolve(x, h / h.sum(), mode="same")


def resample_to_8k(x, rate):
    if rate == OUT_RATE:
        return x
    x = lowpass(x, rate)
    if rate % OUT_RATE == 0:
        return x[:: rate // OUT_RATE]
    t_out = np.arange(int(len(x) * OUT_RATE / rate)) / OUT_RATE
    return np.interp(t_out, np.arange(len(x)) / rate, x)


def trim(x, margin_s=0.04):
    loud = np.nonzero(np.abs(x) > SILENCE * max(np.max(np.abs(x)), 1e-9))[0]
    if len(loud) == 0:
        return x[:0]
    m = int(margin_s * OUT_RATE)
    return x[max(loud[0] - m, 0): loud[-1] + m + 1]


SEG_UEND = np.array([0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF])


def mulaw_encode(x):
    """G.711 mu-law (Sun's reference g711.c; ffmpeg's table differs by one step on a few values): float -1..1 in, uint8 out."""
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype(np.int32) >> 2
    mask = np.where(pcm < 0, 0x7F, 0xFF)
    v = np.minimum(np.abs(pcm), 8159) + 0x21
    seg = np.searchsorted(SEG_UEND, v, side="left")
    u = np.where(seg >= 8, 0x7F, (seg << 4) | ((v >> (np.minimum(seg, 7) + 1)) & 0x0F))
    return (u ^ mask).astype(np.uint8)


def text_tag(voice, text):
    digest = hashlib.sha256(" ".join(text.split()).encode()).hexdigest()[:16]
    return f"redial-tts voice={voice} text={digest}"


def _chunk(cid, payload):
    return cid + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) % 2 else b"")


def wav_mulaw(data, tag=""):
    """WAVE_FORMAT_MULAW with a fact chunk (the layout ffmpeg writes), plus an
    optional LIST/INFO ICMT tag before the audio."""
    n = len(data)
    fmt = struct.pack("<HHIIHHH", 7, 1, OUT_RATE, OUT_RATE, 1, 8, 0)
    info = _chunk(b"LIST", b"INFO" + _chunk(b"ICMT", tag.encode() + b"\0")) if tag else b""
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt +
            b"fact" + struct.pack("<II", 4, n) + info + b"data" + struct.pack("<I", n) + data.tobytes())
    if n % 2:
        body += b"\0"
    return b"RIFF" + struct.pack("<I", len(body)) + body


def to_phone_wav(samples, rate, tag=""):
    x = np.asarray(samples, dtype=np.float64)
    x = trim(resample_to_8k(x, rate))
    if len(x) == 0:
        raise ValueError("the text produced no audio")
    x = x * (PEAK / max(np.max(np.abs(x)), 1e-9))
    return wav_mulaw(mulaw_encode(x), tag)


def speakable(text):
    """What the model is given: dial codes written 2-1-1 are read as digits, not
    "two dash one dash one". The tag is still made from the text as sent."""
    return re.sub(r"(?<=\d)-(?=\d)", " ", text)


def clean_text(text):
    text = "".join(c for c in str(text) if c.isprintable() or c in " \n\t").strip()
    return " ".join(text.split())


# -- the model --------------------------------------------------------------

class Renderer:
    def __init__(self, models_dir, voice, cache_dir=None):
        from kokoro_onnx import Kokoro
        models_dir = Path(models_dir)
        self.kokoro = Kokoro(str(models_dir / MODEL_FILES[0]), str(models_dir / MODEL_FILES[1]))
        self.voice = voice
        self.lang = "en-gb" if voice.startswith("b") else "en-us"
        self.cache = Path(cache_dir) if cache_dir else None
        if self.cache:
            self.cache.mkdir(parents=True, exist_ok=True)

    def render(self, text):
        """Returns (wav bytes, True if it came from the cache)."""
        text = clean_text(text)
        if not text:
            raise ValueError("empty text")
        if len(text) > MAX_TEXT:
            raise ValueError(f"text is longer than {MAX_TEXT} characters")
        key = hashlib.sha256(f"v2\n{self.voice}\n{text}".encode()).hexdigest()
        cached = self.cache / f"{key}.wav" if self.cache else None
        if cached and cached.exists():
            os.utime(cached)
            return cached.read_bytes(), True
        samples, rate = self.kokoro.create(speakable(text), voice=self.voice, speed=1.0, lang=self.lang)
        wav = to_phone_wav(samples, rate, text_tag(self.voice, text))
        if cached:
            with tempfile.NamedTemporaryFile(dir=self.cache, delete=False) as f:
                f.write(wav)
            os.replace(f.name, cached)
            self.prune()
        return wav, False

    def prune(self):
        files = sorted(self.cache.glob("*.wav"), key=lambda p: p.stat().st_mtime)
        for p in files[:-CACHE_MAX_FILES]:
            p.unlink(missing_ok=True)


def make_handler(renderer):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "redial-tts"
        sys_version = ""

        def send(self, status, data, ctype, extra=None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def send_json(self, status, obj):
            self.send(status, json.dumps(obj).encode(), "application/json")

        def do_GET(self):
            if self.path == "/health":
                return self.send_json(200, {"ok": True, "voice": renderer.voice})
            self.send_json(404, {"error": "not_found"})

        def do_POST(self):
            if self.path != "/synthesize":
                return self.send_json(404, {"error": "not_found"})
            try:
                length = int(self.headers.get("Content-Length") or 0)
                if length > 8192:
                    return self.send_json(413, {"error": "too_large", "message": "Request too large"})
                body = json.loads(self.rfile.read(length) or b"{}")
                wav, hit = renderer.render(body.get("text", ""))
            except ValueError as e:
                return self.send_json(400, {"error": "bad_text", "message": str(e)})
            except Exception as e:  # keep the service up; details stay in the log
                self.log_error("render failed: %r", e)
                return self.send_json(500, {"error": "internal", "message": "Rendering failed"})
            self.send(200, wav, "audio/wav", {"X-Redial-Cache": "hit" if hit else "miss"})

        def log_message(self, fmt, *args):
            sys.stderr.write("%s\n" % (fmt % args))

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("serve", "render"):
        p = sub.add_parser(name)
        p.add_argument("--models", required=True, help="folder with " + " and ".join(MODEL_FILES))
        p.add_argument("--voice", default="af_sarah")
        p.add_argument("--cache", help="cache folder")
    sub.choices["serve"].add_argument("--port", type=int, default=8078)
    sub.choices["render"].add_argument("text")
    sub.choices["render"].add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    renderer = Renderer(args.models, args.voice, args.cache)
    if args.cmd == "render":
        Path(args.out).write_bytes(renderer.render(args.text)[0])
        print(f"wrote {args.out}")
        return
    srv = http.server.HTTPServer(("127.0.0.1", args.port), make_handler(renderer))
    print(f"redial-tts listening on 127.0.0.1:{args.port}, voice {args.voice}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
