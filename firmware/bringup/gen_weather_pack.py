#!/usr/bin/env python3
"""Builds the weather report's voice pack (dial 1212): every word and phrase the
report strings together, spoken once, in one file the board downloads.

  ./gen_weather_pack.py                     # all clips, default voice (Kokoro af_sarah)
  ./gen_weather_pack.py --kokoro-voice bf_emma
  ./gen_weather_pack.py --upload phone.local   # also put it straight on a board

Writes firmware/assets/weather/weather_en.pak and weather_en.json. The pack is one
ordinary WAV (8 kHz mono 8-bit mu-law) holding every clip back to back, plus a
"wxix" chunk that says where each clip starts and how long it is:

    AR-WX 1
    <name> <offset> <length>        (one line per clip, bytes from the start of "data")

so the board stores a single file (its file lists stop at 100 entries) and plays
pieces of it. weather_en.json gives the pack's size and SHA-256; the board checks
both after downloading. Run it again whenever a clip's text changes, then commit
both files: boards fetch them from the main branch on GitHub.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import gen_prompt

OUT = Path(__file__).resolve().parent.parent / "assets" / "weather"
PACK_VERSION = 1
PEAK_DB = -1.0
TRIM = ("silenceremove=start_periods=1:start_threshold=-50dB,"
        "areverse,silenceremove=start_periods=1:start_threshold=-50dB,areverse")

ONES = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
        "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
        "nineteen"]
TENS = ["", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"]


def number_words(n: int) -> str:
    if n < 20:
        return ONES[n]
    if n < 100:
        return TENS[n // 10] + ("-" + ONES[n % 10] if n % 10 else "")
    return "one hundred" + (" " + number_words(n - 100) if n > 100 else "")


# name -> text. Names must match weather.c. Trailing punctuation shapes how each
# piece is said, so the joins sound like one sentence.
PHRASES = {
    "intro": "Here's the weather.",
    "now": "Right now,",
    "and": "and",
    "degrees": "degrees.",
    "today": "Today,",
    "tonight": "Tonight,",
    "tomorrow": "Tomorrow,",
    "high": "with a high of",
    "low": "and a low of",
    "low_only": "a low of",
    "pct_rain": "percent chance of rain.",
    "pct_snow": "percent chance of snow.",
    "minus": "minus",
    "unavailable": "Sorry, the weather report isn't available right now. Please try again in a few minutes.",
    # Sky and precipitation, one per group of Open-Meteo's WMO weather codes (weather.c).
    "c_clear": "clear skies,",
    "c_mostly_clear": "mostly clear,",
    "c_partly_cloudy": "partly cloudy,",
    "c_cloudy": "cloudy,",
    "c_fog": "foggy,",
    "c_drizzle": "drizzle,",
    "c_freezing_drizzle": "freezing drizzle,",
    "c_light_rain": "light rain,",
    "c_rain": "rain,",
    "c_heavy_rain": "heavy rain,",
    "c_freezing_rain": "freezing rain,",
    "c_light_snow": "light snow,",
    "c_snow": "snow,",
    "c_heavy_snow": "heavy snow,",
    "c_showers": "rain showers,",
    "c_heavy_showers": "heavy rain showers,",
    "c_snow_showers": "snow showers,",
    "c_thunder": "thunderstorms,",
    "c_thunder_hail": "thunderstorms with hail,",
}
NUMBERS = {f"n{i}": number_words(i) for i in range(0, 111)}


def ffmpeg(args: list[str]) -> str:
    r = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-y", *args], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"ffmpeg failed: {r.stderr[-400:]}")
    return r.stderr


def render(text: str, voice: str, engine: str, td: Path) -> bytes:
    """One clip as raw 8 kHz mu-law bytes, silence trimmed, peak just under full scale."""
    raw = gen_prompt.synthesize(text, engine, None, "en-us", "Samantha", td / "raw.wav", True, voice)
    filters = "aresample=8000," + TRIM
    out = ffmpeg(["-i", str(raw), "-af", filters + ",volumedetect", "-f", "null", "-"])
    m = re.search(r"max_volume:\s*(-?[\d.]+) dB", out)
    gain = PEAK_DB - float(m.group(1)) if m else 0.0
    dst = td / "clip.raw"
    ffmpeg(["-i", str(raw), "-af", f"{filters},volume={gain:.2f}dB", "-ac", "1", "-ar", "8000",
            "-c:a", "pcm_mulaw", "-f", "mulaw", str(dst)])
    return dst.read_bytes()


def chunk(tag: bytes, body: bytes) -> bytes:
    return tag + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def build_pack(clips: dict[str, bytes]) -> bytes:
    data, index = bytearray(), [f"AR-WX {PACK_VERSION}"]
    for name, audio in clips.items():
        index.append(f"{name} {len(data)} {len(audio)}")
        data += audio
    fmt = struct.pack("<HHIIHH", 7, 1, 8000, 8000, 1, 8)   # mu-law, mono, 8 kHz, 8 bits
    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"wxix", ("\n".join(index) + "\n").encode()) + chunk(b"data", bytes(data))
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default="kokoro", choices=("auto", "kokoro", "piper"))
    ap.add_argument("--kokoro-voice", default=gen_prompt.DEFAULT_KOKORO_VOICE)
    ap.add_argument("--upload", metavar="HOST", help="also upload the pack to this board")
    args = ap.parse_args()

    clips: dict[str, bytes] = {}
    jobs = {**PHRASES, **NUMBERS}
    with tempfile.TemporaryDirectory() as td:
        for i, (name, text) in enumerate(jobs.items(), 1):
            clips[name] = render(text, args.kokoro_voice, args.engine, Path(td))
            print(f"[{i}/{len(jobs)}] {name}: {text!r} ({len(clips[name])} bytes)", flush=True)

    pack = build_pack(clips)
    OUT.mkdir(parents=True, exist_ok=True)
    pak = OUT / "weather_en.pak"
    pak.write_bytes(pack)
    manifest = {"version": PACK_VERSION, "lang": "en", "clips": len(clips), "size": len(pack),
                "sha256": hashlib.sha256(pack).hexdigest(), "voice": args.kokoro_voice}
    (OUT / "weather_en.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"wrote {pak} ({len(pack)} bytes, {len(pack) / 8000:.0f} s of speech) and weather_en.json")
    if args.upload:
        gen_prompt.upload(pak, args.upload, pak.name)


if __name__ == "__main__":
    main()
