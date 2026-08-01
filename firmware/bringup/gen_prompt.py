#!/usr/bin/env python3
"""Generate a prompt/clip WAV (text-to-speech or an existing audio file,
converted to the format the device's `player` component expects) and
optionally upload it straight to the phone over HTTP.

The ESP32 has no TTS of its own -- this is the host-side half of milestone 3
(FEATURES.md): produce mono 16-bit PCM WAV. Defaults to 8000 Hz -- real
telephone bandwidth is ~300-3400Hz, so 8kHz (Nyquist to 4kHz) is genuine
phone-quality audio, not a compromise -- it's also what G.711 (the VoIP
milestone's eventual codec) uses. 8kHz vs. this device's native 48kHz I2S
rate is a 6x size win (storage is capped at 4MB total on the device) with no
decoder needed: player.c resamples on playback, so the device's live
telephony audio (still 48kHz) is untouched. Pass --rate 48000 if you'd
rather skip resampling for some reason.

Examples:
    # Text-to-speech (macOS `say`), saved locally only:
    ./gen_prompt.py "For assistance, please hang up and try again." --name operator.wav

    # Same, uploaded straight to the phone:
    ./gen_prompt.py "For assistance, please hang up and try again." \\
        --name operator.wav --upload phone.local

    # Convert an existing recording instead of synthesizing speech:
    ./gen_prompt.py --from banana_song.mp3 --name banana_phone.wav --upload phone.local

Well-known names the firmware looks for: operator.wav (dial 0, fixed). The
401/402/403/404/411/412/413/414 "silly clip" codes are NOT fixed filenames
any more -- they're a mutable code<->filename table seeded from
dialplan/dialplan_defaults.h (see CLIP_DEFAULTS there, and CLIP_MAP in
sync_pi_audio.py) and editable at runtime from Settings -> Dial Codes.
Match whatever filename that table currently points a code at, or add a new
code/filename pair from the web UI after uploading -- no firmware rebuild
needed for that part.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

DEFAULT_RATE = 8000
DEFAULT_VOICE = "Samantha"


def synthesize(text: str, voice: str, aiff_path: Path) -> None:
    if shutil.which("say") is None:
        raise SystemExit(
            "'say' not found -- this text-to-speech path is macOS-only. "
            "Use --from <audio file> instead on other platforms."
        )
    subprocess.run(["say", "-v", voice, "-o", str(aiff_path), text], check=True)


def convert_to_wav(src: Path, dst: Path, rate: int) -> None:
    if shutil.which("afconvert") is not None:
        # Linear PCM, 16-bit little-endian, mono, at `rate` Hz -- exactly
        # what player.c's wav_parse() wants.
        subprocess.run(
            ["afconvert", "-f", "WAVE", "-d", f"LEI16@{rate}", "-c", "1",
             str(src), str(dst)],
            check=True,
        )
        return
    if shutil.which("ffmpeg") is not None:
        subprocess.run(
            ["ffmpeg", "-y", "-i", str(src), "-ar", str(rate), "-ac", "1",
             "-sample_fmt", "s16", str(dst)],
            check=True, capture_output=True,
        )
        return
    raise SystemExit(
        "Need 'afconvert' (macOS, built in) or 'ffmpeg' on PATH to convert audio."
    )


def trim_wav(path: Path, max_seconds: float, rate: int) -> None:
    """Truncate a mono 16-bit PCM WAV's data chunk in place. Some of the
    original pi_rotary_phone audio files are multi-minute songs (the Pi just
    streamed them until hangup) -- this device's storage cap is 4MB total, so
    anything more than a short sting needs trimming before upload."""
    raw = bytearray(path.read_bytes())
    if raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE file, can't trim")

    off = 12
    data_off = None
    while off + 8 <= len(raw):
        chunk_id = bytes(raw[off:off + 4])
        chunk_sz = int.from_bytes(raw[off + 4:off + 8], "little")
        body = off + 8
        if chunk_id == b"data":
            data_off = body
            break
        off = body + chunk_sz + (chunk_sz & 1)
    if data_off is None:
        raise SystemExit(f"{path}: couldn't find 'data' chunk to trim")

    max_bytes = int(max_seconds * rate) * 2  # mono, 16-bit
    max_bytes -= max_bytes % 2
    data_len = len(raw) - data_off
    if data_len <= max_bytes:
        return  # already short enough

    new_len = data_off + max_bytes
    raw[data_off - 4:data_off] = max_bytes.to_bytes(4, "little")  # this data chunk's size field
    raw[4:8] = (new_len - 8).to_bytes(4, "little")  # RIFF chunk size field
    path.write_bytes(raw[:new_len])


def upload(path: Path, host: str, name: str) -> None:
    if not host.startswith("http://") and not host.startswith("https://"):
        host = f"http://{host}"
    url = f"{host.rstrip('/')}/media/{name}"
    data = path.read_bytes()
    req = urllib.request.Request(url, data=data, method="POST",
                                  headers={"Content-Type": "audio/wav"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        body = resp.read().decode("utf-8", errors="replace").strip()
    print(f"uploaded -> {url} ({len(data)} bytes): {body}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src_group = p.add_mutually_exclusive_group(required=True)
    src_group.add_argument("text", nargs="?", help="Text to speak (macOS 'say')")
    src_group.add_argument("--from", dest="from_file", metavar="FILE",
                            help="Convert an existing audio file instead of synthesizing speech")
    p.add_argument("--name", required=True,
                    help="Output filename, e.g. operator.wav or clip_401.wav (see module docstring)")
    p.add_argument("--voice", default=DEFAULT_VOICE, help=f"macOS 'say' voice (default: {DEFAULT_VOICE})")
    p.add_argument("--rate", type=int, default=DEFAULT_RATE,
                    help=f"Output sample rate in Hz -- must match the device's audio_sample_rate() "
                         f"(default: {DEFAULT_RATE})")
    p.add_argument("--out-dir", default=".", help="Where to save the WAV locally (default: cwd)")
    p.add_argument("--max-seconds", type=float, default=None,
                    help="Trim to at most this many seconds (from the start) -- storage is capped "
                         "at 4MB total on the device, so multi-minute source files need this")
    p.add_argument("--upload", metavar="HOST",
                    help="Also POST the result to http://HOST/media/<name>, e.g. --upload phone.local "
                         "or --upload 192.168.0.148")
    args = p.parse_args()

    out_path = Path(args.out_dir) / args.name
    out_path.parent.mkdir(parents=True, exist_ok=True)

    if args.from_file:
        convert_to_wav(Path(args.from_file), out_path, args.rate)
    else:
        with tempfile.TemporaryDirectory() as td:
            aiff_path = Path(td) / "speech.aiff"
            synthesize(args.text, args.voice, aiff_path)
            convert_to_wav(aiff_path, out_path, args.rate)

    if args.max_seconds:
        trim_wav(out_path, args.max_seconds, args.rate)

    size = out_path.stat().st_size
    print(f"wrote {out_path} ({size} bytes, {args.rate} Hz mono 16-bit PCM)")

    if args.upload:
        upload(out_path, args.upload, args.name)
    else:
        print(f"to upload later:  curl -X POST --data-binary @{out_path} "
              f"http://phone.local/media/{args.name}")


if __name__ == "__main__":
    main()
