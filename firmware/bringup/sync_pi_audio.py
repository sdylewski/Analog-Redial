#!/usr/bin/env python3
"""Batch-convert the pi_rotary_phone Pi's audio clips into the 8 kHz mono
16-bit PCM WAVs this firmware's dialplan expects (ring_of_fire.wav etc. --
see CLIP_MAP below, which mirrors dialplan_defaults.h), and optionally
upload them all to a phone in one command.

This is the "copy the Pi's .wav files over" tool -- for one file at a time,
or to synthesize a new prompt from text, use gen_prompt.py directly (this
script is a thin batch wrapper around the same conversion/upload code).

Source directory defaults to where the Pi's Telephone/Audio lives when its
home directory is mounted locally (see CLAUDE.md); override with --src for
any other checkout/mount.

Examples:
    # Convert only, write to firmware/assets/ for inspection:
    ./sync_pi_audio.py --src /Volumes/scott/Telephone/Audio

    # Convert AND upload straight to a booted phone:
    ./sync_pi_audio.py --src /Volumes/scott/Telephone/Audio --upload phone.local

    # Italian clip variants where they exist (thats_amore/crocfa instead of
    # ringfire/crocodile_rock), plus the Italian operator script:
    ./sync_pi_audio.py --src /Volumes/scott/Telephone/Audio --lang it --upload phone.local

Storage note: clip audio and operator.wav are never auto-evicted by the
device's storage cap (see storage.c) -- only files named rec_* (actual
recordings) are, so uploading these once is durable across normal use.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from gen_prompt import convert_to_wav, trim_wav, synthesize, upload, DEFAULT_SAY_VOICE  # noqa: E402

# dest name -> (english source, italian source or None if no separate dub).
# Dest names must match dialplan_defaults.h's CLIP_DEFAULTS -- that's what
# actually gets seeded into the device's mutable clip table on first boot,
# not a fixed compile-time filename convention (the old clip_401.wav-style
# names this used to target were retired when that table became editable).
CLIP_MAP: dict[str, tuple[str, str | None]] = {
    "ring_of_fire.wav":    ("ringfire.wav", "thats_amore.wav"),   # Ring of Fire / That's Amore
    "burp.wav":            ("burp.wav", None),                     # Burp
    "crocodile_rock.wav":  ("crocodile_rock.wav", "crocfa.wav"),   # Crocodile Rock / Crocfa
    "martians.wav":        ("martians.wav", None),                 # Martians
    # banana_phone/boat/split.wav, lime_in_coconut.wav dropped -- their
    # pi_rotary_phone source audio was never part of this checkout and
    # isn't coming (dial codes 411/413/414 retired; 412 repurposed for a
    # different, non-Pi source -- see dialplan_defaults.h).
}

# Operator help script text, read from the Pi's own prompt files (see
# pi_rotary_phone/operator_help_<lang>.txt) and TTS'd with gen_prompt's
# `say` path -- there's no pre-recorded operator.wav on the Pi to copy since
# it generates this one live.
OPERATOR_SCRIPT_NAME = {"en": "operator_help_en.txt", "it": "operator_help_it.txt"}

DEFAULT_MAX_SECONDS = 20.0  # full songs on the Pi run minutes; a sting is enough here


def find_source(src_dir: Path, name: str) -> Path | None:
    """Case-insensitive filename match -- the Pi's Audio_files/ has some
    ALL-CAPS legacy names alongside the lowercase current ones."""
    exact = src_dir / name
    if exact.is_file():
        return exact
    lower = name.lower()
    for f in src_dir.iterdir():
        if f.is_file() and f.name.lower() == lower:
            return f
    return None


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--src", required=True, metavar="DIR",
                   help="Pi Audio directory to read from, e.g. /Volumes/scott/Telephone/Audio")
    p.add_argument("--lang", default="en", choices=["en", "it"],
                   help="Pick Italian source clips/operator script where a dub exists (default: en)")
    p.add_argument("--rate", type=int, default=8000, help="Output sample rate (default: 8000)")
    p.add_argument("--max-seconds", type=float, default=DEFAULT_MAX_SECONDS,
                   help=f"Trim each clip to at most this long (default: {DEFAULT_MAX_SECONDS}s) -- "
                        "the Pi's originals are often full songs")
    p.add_argument("--out-dir", default=str(Path(__file__).parent.parent / "assets"),
                   help="Where to write converted WAVs locally (default: firmware/assets/)")
    p.add_argument("--upload", metavar="HOST",
                   help="Also POST every converted file to http://HOST/media/<name>, e.g. phone.local")
    p.add_argument("--skip-operator", action="store_true",
                   help="Don't synthesize operator.wav (skip if 'say' unavailable / not macOS)")
    p.add_argument("--voice", default=DEFAULT_SAY_VOICE, help="macOS 'say' voice for the operator script")
    args = p.parse_args()

    src_dir = Path(args.src)
    if not src_dir.is_dir():
        raise SystemExit(f"--src {src_dir} is not a directory")
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    ok, missing = [], []

    for dest_name, (en_src, it_src) in CLIP_MAP.items():
        src_name = it_src if (args.lang == "it" and it_src) else en_src
        src_path = find_source(src_dir, src_name)
        if not src_path:
            missing.append((dest_name, src_name))
            continue
        dst_path = out_dir / dest_name
        convert_to_wav(src_path, dst_path, args.rate)
        trim_wav(dst_path, args.max_seconds, args.rate)
        size = dst_path.stat().st_size
        print(f"  {src_path.name:28s} -> {dest_name:14s} ({size:,} bytes)")
        ok.append(dst_path)

    if not args.skip_operator:
        script_name = OPERATOR_SCRIPT_NAME[args.lang]
        # operator_help_<lang>.txt lives alongside telephone.py on the Pi,
        # one directory up from Audio/ in the usual Telephone/ layout.
        script_path = src_dir.parent / script_name
        if script_path.is_file():
            text = script_path.read_text(encoding="utf-8").strip()
            dst_path = out_dir / "operator.wav"
            try:
                import tempfile
                with tempfile.TemporaryDirectory() as td:
                    raw = synthesize(text, "say", None, "en-us", args.voice,
                                      Path(td) / "speech.wav")
                    convert_to_wav(raw, dst_path, args.rate)
                size = dst_path.stat().st_size
                print(f"  {script_name:28s} -> operator.wav   ({size:,} bytes, TTS)")
                ok.append(dst_path)
            except SystemExit as exc:
                print(f"  (skipped operator.wav: {exc})")
        else:
            print(f"  (skipped operator.wav: {script_path} not found)")

    print(f"\n{len(ok)} file(s) converted to {out_dir}/")
    if missing:
        print(f"{len(missing)} not found in {src_dir}:")
        for dest_name, src_name in missing:
            print(f"  {dest_name} <- {src_name}")

    if args.upload and ok:
        print(f"\nUploading to {args.upload} ...")
        for path in ok:
            upload(path, args.upload, path.name)
    elif ok:
        print(f"\nTo upload later:  for f in {out_dir}/*.wav; do "
              f"curl -X POST --data-binary @\"$f\" http://phone.local/media/$(basename \"$f\"); done")


if __name__ == "__main__":
    main()
