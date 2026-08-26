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

Text-to-speech uses Piper (preferred, neural, cross-platform -- the same
engine + voice pi_rotary_phone's install-piper.sh sets up, since that's
been the better-sounding option there). --engine auto (the default) sets
Piper up for you the first time it's needed: `pip install piper-tts` if
the `piper` command isn't on PATH, then downloads the same default voice
the Pi uses (en_US-lessac-medium) into firmware/bringup/piper_voices/ if
no .onnx is there yet. Both steps are skipped once already present, so
this is a one-time cost. Pass --piper-model to use a different voice, or
grab one yourself from https://huggingface.co/rhasspy/piper-voices.

If Piper can't be set up (offline, no pip, etc.) --engine auto falls back
to espeak-ng (`apt install espeak-ng` / `brew install espeak-ng`), then
macOS's `say` -- both noticeably more robotic than Piper, and auto prints
a warning when it lands on either so the drop in quality isn't silent.
--no-auto-install skips the automatic pip install/download and just fails
over to the next engine immediately, if you'd rather not have this script
touch your Python environment or make network calls on its own.

Two ways to use this:

  1. One-shot, same as before: pass text (or --from an existing audio file)
     and --name, optionally --upload HOST to push it straight to a board.

  2. --sync HOST [--secret ...]: ask the board itself what it wants
     generated (Settings -> Operator/Record/Dial Codes -> "Spoken prompt
     text" on each page, POST /prompt_text_set) via GET /api/prompt_requests,
     synthesize everything that's pending, and upload each result back.
     --secret is the same setup password as the VOIP auto-provisioner
     (Settings -> VOIP) -- one shared secret per board, not a new one to
     manage. The board skips checking it entirely for callers on the same
     LAN subnet it's on (its own security model, not something this script
     enforces) -- so --secret is only actually required when this runs
     somewhere else on the network than the board, e.g. over a VPN into a
     remotely-hosted phone. This is otherwise the no-server-dependency path:
     nothing but this script and a local TTS engine, so a board running on
     someone else's Asterisk server doesn't need access to *your*
     infrastructure to generate its own prompts.

Examples:
    # Text-to-speech (Piper/espeak-ng, auto-picked), saved locally only:
    ./gen_prompt.py "For assistance, please hang up and try again." --name operator.wav

    # Same, uploaded straight to the phone:
    ./gen_prompt.py "For assistance, please hang up and try again." \\
        --name operator.wav --upload phone.local

    # Generate + upload everything the board has pending text for, run from
    # the same LAN the board's on (no --secret needed):
    ./gen_prompt.py --sync phone.local

    # Same, but the board's on a different network (VPN, remote setup):
    ./gen_prompt.py --sync phone.local --secret "$(cat setup_password.txt)"

    # Convert an existing recording instead of synthesizing speech:
    ./gen_prompt.py --from banana_song.mp3 --name banana_phone.wav --upload phone.local

Well-known names the firmware looks for: operator.wav (dial 0, fixed),
record_prompt.wav (played before the beep on 211, dial 211). The
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
import json
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

DEFAULT_RATE = 8000
DEFAULT_SAY_VOICE = "Samantha"       # --engine say (macOS only)
DEFAULT_ESPEAK_VOICE = "en-us"       # --engine espeak
PIPER_VOICES_DIR = Path(__file__).resolve().parent / "piper_voices"

# Same voice pi_rotary_phone's scripts/install-piper.sh sets up on the Pi --
# picked there for sounding noticeably less robotic than espeak-ng, at a
# "medium" quality/size tradeoff sane for both a Pi 4 and a one-time laptop
# download. Only used when nothing else is dropped under PIPER_VOICES_DIR/.
DEFAULT_PIPER_VOICE_REL = "en/en_US/lessac/medium/en_US-lessac-medium.onnx"
PIPER_VOICES_BASE_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/main"


def _which(cmd: str) -> str | None:
    return shutil.which(cmd)


def _download_default_piper_voice() -> Path | None:
    """Fetches the same voice pi_rotary_phone uses (DEFAULT_PIPER_VOICE_REL)
    into PIPER_VOICES_DIR. Returns the .onnx path on success, None on any
    failure (offline, HF unreachable, ...) -- callers treat that the same as
    "no model available" and fall back a tier, they don't need to know why."""
    onnx_path = PIPER_VOICES_DIR / Path(DEFAULT_PIPER_VOICE_REL).name
    json_path = onnx_path.with_name(onnx_path.name + ".json")
    try:
        PIPER_VOICES_DIR.mkdir(parents=True, exist_ok=True)
        print(f"no Piper voice found -- downloading the default ({onnx_path.name}, ~60MB)...")
        urllib.request.urlretrieve(f"{PIPER_VOICES_BASE_URL}/{DEFAULT_PIPER_VOICE_REL}", onnx_path)
        urllib.request.urlretrieve(f"{PIPER_VOICES_BASE_URL}/{DEFAULT_PIPER_VOICE_REL}.json", json_path)
    except OSError as e:
        print(f"warning: couldn't download the default Piper voice ({e})", file=sys.stderr)
        onnx_path.unlink(missing_ok=True)
        json_path.unlink(missing_ok=True)
        return None
    print(f"downloaded {onnx_path}")
    return onnx_path


def _resolve_piper_model(explicit: str | None, auto_install: bool) -> Path | None:
    """Explicit --piper-model wins. Otherwise, exactly one *.onnx dropped
    under piper_voices/ next to this script is picked automatically -- more
    than one is ambiguous (which voice?), so that's an error, not a guess.
    With none present, auto_install (default) fetches DEFAULT_PIPER_VOICE_REL
    so a first run works with zero manual setup, matching the Pi's default."""
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            raise SystemExit(f"--piper-model {p} not found")
        return p
    if PIPER_VOICES_DIR.is_dir():
        found = sorted(PIPER_VOICES_DIR.glob("*.onnx"))
        if len(found) == 1:
            return found[0]
        if len(found) > 1:
            names = "\n".join(f"  {f}" for f in found)
            raise SystemExit(
                f"multiple Piper voices in {PIPER_VOICES_DIR}, pass --piper-model to pick one:\n{names}"
            )
    return _download_default_piper_voice() if auto_install else None


def _ensure_piper_cli(auto_install: bool) -> str | None:
    """Returns the `piper` binary's path, installing piper-tts via pip first
    if it's missing and auto_install is set. Returns None (never raises) if
    it's still not available after that -- callers decide what that means
    (hard error for --engine piper, a fallback tier for auto)."""
    found = _which("piper")
    if found or not auto_install:
        return found
    print("Piper not found -- installing via 'pip install piper-tts'...")
    try:
        subprocess.run([sys.executable, "-m", "pip", "install", "piper-tts"], check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"warning: 'pip install piper-tts' failed ({e})", file=sys.stderr)
        return None
    return _which("piper")


def speak_piper(text: str, model_path: Path, out_wav: Path) -> None:
    cmd = ["piper", "--model", str(model_path), "--output_file", str(out_wav)]
    subprocess.run(cmd, input=text.encode("utf-8"), check=True)


def speak_espeak(text: str, voice: str, out_wav: Path) -> None:
    subprocess.run(["espeak-ng", "-v", voice, "-s", "135", "-w", str(out_wav), text], check=True)


def speak_say(text: str, voice: str, out_aiff: Path) -> None:
    subprocess.run(["say", "-v", voice, "-o", str(out_aiff), text], check=True)


def synthesize(text: str, engine: str, piper_model: str | None,
                espeak_voice: str, say_voice: str, raw_path: Path,
                auto_install: bool = True) -> Path:
    """Synthesizes `text` with whichever engine `engine` picks ("auto" tries
    Piper -- installing it and its default voice first if auto_install and
    neither is present yet -- then espeak-ng, then macOS `say` as a last
    resort, warning on stderr when it lands on one of those lower-quality
    fallbacks). Returns the path actually written (raw_path itself, except
    for `say`'s AIFF output) -- still needs convert_to_wav() to reach the
    device's target format."""
    if engine in ("auto", "piper"):
        piper_bin = _ensure_piper_cli(auto_install)
        if piper_bin:
            model = _resolve_piper_model(piper_model, auto_install)
            if model:
                speak_piper(text, model, raw_path)
                return raw_path
            if engine == "piper":
                raise SystemExit(
                    "Piper requested (--engine piper) but no voice model available (auto-download "
                    "failed -- see the warning above). Grab one from "
                    "https://huggingface.co/rhasspy/piper-voices and either pass --piper-model "
                    f"or drop it under {PIPER_VOICES_DIR}/."
                )
            print("warning: Piper installed but no voice model available -- "
                  "falling back to a more robotic-sounding engine.", file=sys.stderr)
        elif engine == "piper":
            raise SystemExit(
                "Piper requested (--engine piper) but not available (install failed or "
                "--no-auto-install was passed). Install it yourself with 'pip install piper-tts'."
            )
        else:
            print("warning: Piper not available -- falling back to a more robotic-sounding "
                  "engine. Run 'pip install piper-tts' yourself, or drop --no-auto-install "
                  "if you didn't want this script installing things automatically.",
                  file=sys.stderr)

    if engine in ("auto", "espeak"):
        if _which("espeak-ng"):
            if engine == "auto":
                print("using espeak-ng (more robotic than Piper) -- see the warning above "
                      "for how to get Piper instead.", file=sys.stderr)
            speak_espeak(text, espeak_voice, raw_path)
            return raw_path
        if engine == "espeak":
            raise SystemExit(
                "espeak-ng requested but not found on PATH -- install it "
                "(apt install espeak-ng / brew install espeak-ng)."
            )

    if engine in ("auto", "say") and _which("say"):
        if engine == "auto":
            print("using macOS 'say' (more robotic than Piper) -- see the warning above "
                  "for how to get Piper instead.", file=sys.stderr)
        aiff_path = raw_path.with_suffix(".aiff")
        speak_say(text, say_voice, aiff_path)
        return aiff_path
    if engine == "say":
        raise SystemExit("--engine say requires macOS's 'say' command, not found here.")

    raise SystemExit(
        "No TTS engine available. Install one of:\n"
        "  pip install piper-tts   (recommended -- this script does this automatically unless\n"
        "                           --no-auto-install was passed)\n"
        "  espeak-ng                (apt install espeak-ng / brew install espeak-ng)\n"
    )


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


def _base_url(host: str) -> str:
    if not host.startswith("http://") and not host.startswith("https://"):
        host = f"http://{host}"
    return host.rstrip("/")


def upload(path: Path, host: str, name: str) -> None:
    url = f"{_base_url(host)}/media/{name}"
    data = path.read_bytes()
    req = urllib.request.Request(url, data=data, method="POST",
                                  headers={"Content-Type": "audio/wav"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        body = resp.read().decode("utf-8", errors="replace").strip()
    print(f"uploaded -> {url} ({len(data)} bytes): {body}")


def fetch_prompt_requests(host: str, secret: str | None) -> list[dict]:
    url = f"{_base_url(host)}/api/prompt_requests"
    if secret:
        url += f"?secret={urllib.parse.quote(secret)}"
    try:
        with urllib.request.urlopen(url, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise SystemExit(
                f"{host} rejected this request ({e.code}) -- if this script isn't running on "
                "the same LAN subnet as the board, pass --secret (Settings -> VOIP's setup "
                "password on the board)."
            ) from e
        raise


def sync_prompts(host: str, secret: str | None, engine: str, piper_model: str | None,
                  espeak_voice: str, say_voice: str, rate: int, out_dir: Path,
                  only: list[str] | None, auto_install: bool = True) -> None:
    entries = fetch_prompt_requests(host, secret)
    pending = [e for e in entries if e.get("pending") and e.get("text")]
    if only:
        pending = [e for e in pending if e["name"] in only]
    if not pending:
        print("nothing to generate -- all tracked prompts are up to date "
              "(or none have text set yet -- see Settings -> Operator/Record/Dial Codes)")
        return

    out_dir.mkdir(parents=True, exist_ok=True)
    for e in pending:
        name, text = e["name"], e["text"]
        print(f"generating '{name}': {text!r}")
        out_path = out_dir / name
        with tempfile.TemporaryDirectory() as td:
            raw = synthesize(text, engine, piper_model, espeak_voice, say_voice,
                              Path(td) / "speech.wav", auto_install)
            convert_to_wav(raw, out_path, rate)
        upload(out_path, host, name)
    print(f"synced {len(pending)} prompt(s)")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("text", nargs="?", help="Text to speak")
    p.add_argument("--from", dest="from_file", metavar="FILE",
                    help="Convert an existing audio file instead of synthesizing speech")
    p.add_argument("--name",
                    help="Output filename, e.g. operator.wav or clip_401.wav (see module "
                         "docstring). Required unless --sync.")
    p.add_argument("--engine", default="auto", choices=("auto", "piper", "espeak", "say"),
                    help="TTS engine (default: auto -- Piper, then espeak-ng, then macOS 'say')")
    p.add_argument("--piper-model", help="Path to a Piper .onnx voice model")
    p.add_argument("--espeak-voice", default=DEFAULT_ESPEAK_VOICE,
                    help=f"espeak-ng voice (default: {DEFAULT_ESPEAK_VOICE})")
    p.add_argument("--voice", dest="say_voice", default=DEFAULT_SAY_VOICE,
                    help=f"macOS 'say' voice, only used with --engine say (default: {DEFAULT_SAY_VOICE})")
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
    p.add_argument("--sync", metavar="HOST",
                    help="Instead of a one-shot generate: fetch pending prompt text from the board "
                         "(GET /api/prompt_requests), generate + upload audio for each. --secret is "
                         "only needed if this isn't running on the board's own LAN subnet. Ignores "
                         "text/--from/--name/--upload/--max-seconds.")
    p.add_argument("--secret", help="Setup password for --sync (Settings -> VOIP on the board -- "
                                     "same shared secret as the auto-provisioner). Not needed when "
                                     "running --sync from the same LAN subnet as the board -- only "
                                     "for a board reachable over a different network (VPN, remote "
                                     "setup, etc).")
    p.add_argument("--only", action="append", metavar="NAME",
                    help="With --sync, only generate this clip name (repeatable). Default: everything pending.")
    p.add_argument("--no-auto-install", action="store_true",
                    help="Don't let --engine auto/piper run 'pip install piper-tts' or download a "
                         "voice model on your behalf -- fail over to the next engine immediately "
                         "instead (or, with --engine piper, error out) if Piper isn't already set up.")
    args = p.parse_args()
    auto_install = not args.no_auto_install

    if args.sync:
        sync_prompts(args.sync, args.secret, args.engine, args.piper_model,
                     args.espeak_voice, args.say_voice, args.rate, Path(args.out_dir), args.only,
                     auto_install)
        return

    if args.text and args.from_file:
        p.error("text and --from are mutually exclusive")
    if not args.text and not args.from_file:
        p.error("one of: text, --from FILE, or --sync HOST is required")
    if not args.name:
        p.error("--name is required")

    out_path = Path(args.out_dir) / args.name
    out_path.parent.mkdir(parents=True, exist_ok=True)

    if args.from_file:
        convert_to_wav(Path(args.from_file), out_path, args.rate)
    else:
        with tempfile.TemporaryDirectory() as td:
            raw = synthesize(args.text, args.engine, args.piper_model,
                              args.espeak_voice, args.say_voice, Path(td) / "speech.wav",
                              auto_install)
            convert_to_wav(raw, out_path, args.rate)

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
