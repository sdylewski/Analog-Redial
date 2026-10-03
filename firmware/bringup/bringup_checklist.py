#!/usr/bin/env python3
"""Run the full bring-up sequence against a connected board: MCU -> codec ->
audio -> SLIC -> DTMF, in the order each step depends on the last. Stops at
the first failing step so you fix that block before moving on.

Each step is also runnable standalone (test_mcu.py, test_codec.py, ...) once
you know which block is broken.
"""

from __future__ import annotations
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

STEPS = [
    ("MCU / console", "test_mcu.py", []),
    ("Codec", "test_codec.py", []),
    ("Audio", "test_audio.py", ["--line", "0"]),
    ("SLIC / line 0", "test_slic.py", ["--line", "0"]),
    ("DTMF / tones", "test_dtmf.py", ["--line", "0"]),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port, forwarded to every step")
    parser.add_argument("--yes", action="store_true",
                         help="don't pause for confirmation between steps (for CI-style runs)")
    args = parser.parse_args()

    common = ["--port", args.port] if args.port else []

    for i, (label, script, extra) in enumerate(STEPS, 1):
        print(f"\n{'=' * 60}\n{i}/{len(STEPS)}: {label}\n{'=' * 60}")
        if not args.yes and i > 1:
            input("Press Enter to run this step (Ctrl+C to stop)... ")
        rc = subprocess.call([sys.executable, str(HERE / script), *common, *extra])
        if rc != 0:
            print(f"\nSTOP: '{label}' failed (exit {rc}). Fix this block before continuing.")
            return rc

    print("\nAll steps ran. Steps marked 'Manual check required' still need a human to confirm "
          "sound/behavior -- scroll back through the output above.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
