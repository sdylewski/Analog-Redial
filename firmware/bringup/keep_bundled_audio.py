#!/usr/bin/env python3
"""Copy clips out of a board's built-in audio before a reflash removes them.

The "bundled" partition is rewritten by every `idf.py flash`, and since
2026-10-02 it only carries operator.wav, am_greeting.wav and dialup.mp3. A
board flashed before that also has clips like burp.wav baked in, and the
next flash would drop them. This copies them into the board's writable
storage, which flashing never touches -- the board keeps playing them, and
its dial codes for them keep working.

Run it once per older board, before reflashing:

  ./keep_bundled_audio.py --host telephono.local
  ./keep_bundled_audio.py --host 192.168.0.235 --names burp.wav my_clip.wav

Uses curl (macOS blocks Python sockets to LAN hosts without Local Network
permission). Re-running is harmless: a clip already in writable storage is
just copied over itself. Saved clips are never auto-deleted: storage only
evicts recordings (rec_*, voicemail_*).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

# Everything the old whole-directory bundle could have carried.
FORMERLY_BUNDLED = ["burp.wav", "crocodile_rock.wav", "martians.wav", "ring_of_fire.wav"]


def curl(args: list[str]) -> int:
    proc = subprocess.run(["curl", "-sS", "-m", "60", "-w", "%{http_code}", *args],
                          capture_output=True, text=True)
    try:
        return int(proc.stdout.strip()[-3:])
    except ValueError:
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="Board hostname or IP")
    ap.add_argument("--names", nargs="+", default=FORMERLY_BUNDLED,
                    help=f"Clips to keep (default: {' '.join(FORMERLY_BUNDLED)})")
    args = ap.parse_args()
    base = args.host if args.host.startswith("http") else f"http://{args.host}"

    kept, absent, failed = [], [], []
    with tempfile.TemporaryDirectory() as tmp:
        for name in args.names:
            local = Path(tmp) / name
            code = curl(["-o", str(local), f"{base}/media/{name}"])
            if code != 200 or not local.exists() or local.stat().st_size == 0:
                absent.append(name)
                continue
            code = curl(["-o", "/dev/null", "-X", "POST", "-H", "Content-Type: application/octet-stream",
                         "--data-binary", f"@{local}", f"{base}/media/{name}"])
            if 200 <= code < 400:
                kept.append(f"{name} ({local.stat().st_size // 1024} KB)")
            else:
                failed.append(f"{name} (HTTP {code})")

    print("kept in writable storage:", ", ".join(kept) or "none")
    print("not on this board:       ", ", ".join(absent) or "none")
    if failed:
        print("FAILED:                   ", ", ".join(failed))
        return 1
    print("Safe to reflash.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
