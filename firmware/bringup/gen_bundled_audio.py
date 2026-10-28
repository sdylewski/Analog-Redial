#!/usr/bin/env python3
"""Regenerates the spoken clips baked into every build (firmware/assets/): operator.wav, operator_2line.wav,
record_prompt.wav, timer_prompt.wav, am_greeting.wav, jenny.wav and the digit clips 0-9 and point.

  ./gen_bundled_audio.py                 # all of them, in the default voice (Kokoro af_sarah)
  ./gen_bundled_audio.py --only operator.wav 3.wav
  ./gen_bundled_audio.py --kokoro-voice bf_emma

Formats: the short prompts are 8 kHz mono 16-bit PCM; the two operator prompts, jenny and the
digits are 8 kHz mono 8-bit mu-law (half the size: both operator clips have to fit the 8 MB
layout's 1 MB bundled partition). Every clip is scaled so its peak sits just
under full scale, like the earlier ones, and the digits are trimmed of leading and trailing
silence. Run `idf.py flash` afterwards: the clips are baked into the `bundled` partition,
which OTA updates don't rewrite. dialup.wav is a recording, not speech, so it isn't touched.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import gen_prompt

ASSETS = Path(__file__).resolve().parent.parent / "assets"
PEAK_DB = -0.5

PROMPTS = {  # name -> (text, format)
    # The board name can't be baked in: these say the default name, "phone".
    "operator.wav": ("Operator. To leave a message, dial 2 1 1. Or dial 8 4 6 for a timer. "
                     "For other features and settings, refer to phone dot local.", "ulaw"),
    "operator_2line.wav": ("Operator. To call the other phone, dial 3 2 1. To leave a message, dial 2 1 1. "
                           "Or dial 8 4 6 for a timer. For other features and settings, refer to phone dot local.", "ulaw"),
    "record_prompt.wav": ("Record your message after the beep, then hang up.", "pcm"),
    "am_greeting.wav": ("Leave a message after the beep.", "pcm"),
    "timer_prompt.wav": ("How many minutes for the timer?", "pcm"),
    "timer_done.wav": ("Your timer is done.", "pcm"),
    "alarm_prompt.wav": ("What time should the alarm ring? Dial four digits: the hour, then the minutes, on a 24 hour clock.", "pcm"),
    "alarm_done.wav": ("This is your alarm.", "pcm"),
    "jenny.wav": ("If you're trying to reach Jenny, she's not home right now. Please call back later.", "ulaw"),
}
DIGITS = {f"{i}.wav": w for i, w in enumerate(
    ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"])}
DIGITS["point.wav"] = "point"

TRIM = ("silenceremove=start_periods=1:start_threshold=-50dB,"
        "areverse,silenceremove=start_periods=1:start_threshold=-50dB,areverse")


def ffmpeg(args: list[str]) -> str:
    r = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-y", *args], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"ffmpeg failed: {r.stderr[-400:]}")
    return r.stderr


def peak_gain_db(wav: Path, filters: str | None) -> float:
    cmd = ["-i", str(wav)] + (["-af", filters] if filters else []) + ["-af", (filters + "," if filters else "") + "volumedetect", "-f", "null", "-"]
    # (-af given twice: ffmpeg keeps the last, so the chain above is the full one)
    out = ffmpeg(cmd)
    m = re.search(r"max_volume:\s*(-?[\d.]+) dB", out)
    return PEAK_DB - float(m.group(1)) if m else 0.0


def render(name: str, text: str, fmt: str, trim: bool, args, td: Path) -> None:
    raw = gen_prompt.synthesize(text, args.engine, None, "en-us", "Samantha", td / "raw.wav",
                                True, args.kokoro_voice)
    filters = "aresample=8000," + (TRIM + "," if trim else "")
    filters = filters.rstrip(",")
    gain = peak_gain_db(raw, filters)
    codec = ["-c:a", "pcm_mulaw"] if fmt == "ulaw" else ["-c:a", "pcm_s16le"]
    ffmpeg(["-i", str(raw), "-af", f"{filters},volume={gain:.2f}dB", "-ac", "1", "-ar", "8000", *codec,
            "-map_metadata", "-1", "-fflags", "+bitexact", str(ASSETS / name)])
    print(f"wrote {name} ({(ASSETS / name).stat().st_size} bytes, gain {gain:+.1f} dB)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", nargs="+", help="just these file names")
    ap.add_argument("--engine", default="auto", choices=("auto", "kokoro", "piper"))
    ap.add_argument("--kokoro-voice", default=gen_prompt.DEFAULT_KOKORO_VOICE)
    args = ap.parse_args()
    jobs = [(n, t, f, False) for n, (t, f) in PROMPTS.items()] + [(n, w, "ulaw", True) for n, w in DIGITS.items()]
    if args.only:
        unknown = set(args.only) - {j[0] for j in jobs}
        if unknown:
            sys.exit(f"unknown: {', '.join(sorted(unknown))}")
        jobs = [j for j in jobs if j[0] in args.only]
    with tempfile.TemporaryDirectory() as td:
        for name, text, fmt, trim in jobs:
            render(name, text, fmt, trim, args, Path(td))


if __name__ == "__main__":
    main()
