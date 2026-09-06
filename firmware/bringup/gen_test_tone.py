#!/usr/bin/env python3
"""Generate a structured, repeatable test-tone WAV for automated audio-quality
checks (dropout/glitch detection over a real call), plus a JSON reference
describing exactly what should be in it.

Why a tone sequence rather than real speech: each segment is a single,
known frequency for a known duration, so a receiver can check "is the right
tone present, at roughly the right level, for roughly the right stretch of
time" with simple per-segment energy math (the same idea as the DTMF
detector's Goertzel check, not real speech-quality analysis) -- and a
dropout shows up unambiguously as a segment that's short, missing, or
misaligned in time, rather than being lost in the normal variability of a
real voice signal.

Usage (see the project's audio-quality test procedure in
firmware/bringup/README.md for the full two-board workflow):

    # Generate locally
    python3 gen_test_tone.py

    # Generate and upload straight to a board as a normal clip
    python3 gen_test_tone.py --upload dualphone.local --name test_tone.wav

The reference JSON (same basename, .json instead of .wav) is what
analyze_test_tone.py needs to check a captured recording against -- keep it
alongside the WAV, it's not meant to be uploaded to the board.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import wave
from pathlib import Path

# Same 9-tone list bringup's `echosweep` console command already cycles
# through (300-3400 Hz spans the classic telephone band) -- reusing it here
# means anyone already familiar with that tool recognizes these frequencies.
SWEEP_HZ = [300, 500, 700, 1000, 1500, 2000, 2500, 3000, 3400]

# Not in SWEEP_HZ, so it can never be mistaken for one of the sweep segments
# -- marks the unambiguous start of the sequence, so a receiver can find
# time-zero in a capture even if it started listening a little early/late
# or the capture has leading silence.
SYNC_HZ = 1200.0
SYNC_MS = 300

TONE_MS = 400
GAP_MS = 100
AMPLITUDE = 0.7  # of int16 full scale -- comfortably under clipping (see
                 # this project's docs/signal-levels.md Sec.1B for why
                 # headroom below "full scale on paper" matters in practice)
SAMPLE_RATE = 8000  # real telephone bandwidth / what actually crosses the
                     # network as G.711 -- see gen_prompt.py's own rationale


def build_segments(repeats: int) -> list[dict]:
    """Returns the ordered list of {label, freq_hz, start_ms, duration_ms}
    segments that make up the clip -- the sync marker once, then the sweep
    list `repeats` times (silence gaps are not their own segments, just the
    space between one segment's end and the next one's start)."""
    segments = []
    t = 0
    segments.append({"label": "sync", "freq_hz": SYNC_HZ, "start_ms": t, "duration_ms": SYNC_MS})
    t += SYNC_MS + GAP_MS
    for rep in range(repeats):
        for hz in SWEEP_HZ:
            segments.append({
                "label": f"rep{rep}_{hz}hz",
                "freq_hz": float(hz),
                "start_ms": t,
                "duration_ms": TONE_MS,
            })
            t += TONE_MS + GAP_MS
    return segments


def render_wav(segments: list[dict], sample_rate: int, amplitude: float) -> bytes:
    """PCM samples for the full clip (silence between/after segments included)."""
    import math

    total_ms = segments[-1]["start_ms"] + segments[-1]["duration_ms"]
    total_samples = int(sample_rate * total_ms / 1000)
    samples = [0] * total_samples

    for seg in segments:
        start_i = int(sample_rate * seg["start_ms"] / 1000)
        n = int(sample_rate * seg["duration_ms"] / 1000)
        freq = seg["freq_hz"]
        # Short linear fade in/out (5ms) so each segment's edges don't click
        # -- a click at every boundary would itself look like the kind of
        # glitch this test is trying to detect, muddying real dropouts.
        fade_n = min(int(sample_rate * 0.005), n // 4)
        for i in range(n):
            if start_i + i >= total_samples:
                break
            env = amplitude
            if i < fade_n:
                env *= i / fade_n
            elif i >= n - fade_n:
                env *= (n - i) / fade_n
            v = env * math.sin(2 * math.pi * freq * i / sample_rate)
            samples[start_i + i] = int(v * 32767)

    return struct.pack(f"<{len(samples)}h", *samples)


def write_wav(path: Path, pcm: bytes, sample_rate: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)


def upload(path: Path, host: str, name: str) -> None:
    import urllib.request

    if not host.startswith("http://") and not host.startswith("https://"):
        host = f"http://{host}"
    url = f"{host.rstrip('/')}/media/{name}"
    data = path.read_bytes()
    req = urllib.request.Request(url, data=data, method="POST",
                                  headers={"Content-Type": "audio/wav"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        body = resp.read().decode("utf-8", errors="replace").strip()
    print(f"uploaded -> {url} ({len(data)} bytes): {body}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", default="test_tone.wav",
                         help="Output filename, and the name it's uploaded as (default: test_tone.wav)")
    parser.add_argument("--out-dir", default=".", help="Where to save the WAV+JSON locally (default: cwd)")
    parser.add_argument("--repeats", type=int, default=3,
                         help="How many times to cycle the 9-tone sweep (default: 3, for redundancy "
                              "against a one-off glitch vs. a real recurring problem)")
    parser.add_argument("--rate", type=int, default=SAMPLE_RATE,
                         help=f"Sample rate (default: {SAMPLE_RATE} -- real telephone/G.711 bandwidth)")
    parser.add_argument("--amplitude", type=float, default=AMPLITUDE,
                         help=f"Peak amplitude, 0-1 of full scale (default: {AMPLITUDE})")
    parser.add_argument("--upload", metavar="HOST",
                         help="Also POST the WAV straight to a board as a normal clip (e.g. dualphone.local)")
    args = parser.parse_args()

    segments = build_segments(args.repeats)
    pcm = render_wav(segments, args.rate, args.amplitude)

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    wav_path = out_dir / args.name
    json_path = out_dir / (Path(args.name).stem + ".json")

    write_wav(wav_path, pcm, args.rate)
    with open(json_path, "w") as f:
        json.dump({
            "sample_rate": args.rate,
            "amplitude": args.amplitude,
            "total_ms": segments[-1]["start_ms"] + segments[-1]["duration_ms"],
            "sync_hz": SYNC_HZ,
            "segments": segments,
        }, f, indent=2)

    total_s = (segments[-1]["start_ms"] + segments[-1]["duration_ms"]) / 1000
    print(f"wrote {wav_path} ({len(pcm)} bytes, {total_s:.1f}s, {len(segments)} segments)")
    print(f"wrote {json_path} (reference for analyze_test_tone.py)")

    if args.upload:
        upload(wav_path, args.upload, args.name)

    return 0


if __name__ == "__main__":
    sys.exit(main())
