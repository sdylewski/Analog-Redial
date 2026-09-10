#!/usr/bin/env python3
"""Bring-up: audio path (tone generation, capture level, loopback).

This can only fully verify itself with a human listening/speaking at the
handset -- the script drives the console commands and prints what to listen
for, plus a basic sanity check that ADC RMS actually moves.

Note (SMD/ES8388 boards): both lines capture simultaneously -- each has its
own dedicated ADC input (LIN1/RIN1, see es8388.h), native on SMD3/SMD4's
wiring and bodge-fixed on SMD2's. 'capture' showing only one line's RMS
meter moving is a real fault to chase (wiring, cable, handset), not an
expected codec limitation -- an earlier revision of this note described a
now-fixed SMD2 hardware bug (a shared ADC mux) as if it were permanent;
that's no longer true on any current board.
"""

from __future__ import annotations
import argparse
import re
import sys
import time

from phone_serial import PhoneConsole, add_common_args, pick_port

RMS_RE = re.compile(r"L0 rms=\s*(\d+)\s+L1 rms=\s*(\d+)")


def parse_rms(lines: list[str]) -> list[tuple[int, int]]:
    out = []
    for line in lines:
        m = RMS_RE.search(line)
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--line", type=int, default=0, choices=[0, 1])
    args = parser.parse_args()

    port = pick_port(args.port)
    print(f"Using port: {port}")

    with PhoneConsole(port, args.baud) as console:
        print(f"\n$ tone 1000 800 {args.line}   (listen for a 1 kHz tone on line {args.line})")
        for line in console.send(f"tone 1000 800 {args.line}"):
            print(" ", line)
        time.sleep(1.0)

        print("\n$ capture 2000   (make some noise near the handset mic)")
        cap_lines = console.send("capture 2000", timeout_s=4.0)
        for line in cap_lines:
            print(" ", line)
        samples = parse_rms(cap_lines)
        if not samples:
            print("\nFAIL: no RMS samples parsed -- audio_init likely failed, check codec bring-up first")
            return 1
        levels = [s[args.line] for s in samples]
        print(f"\nline {args.line} RMS samples: {levels}")
        if max(levels) - min(levels) < 5 and max(levels) == 0:
            print("WARNING: RMS stayed at 0 the whole time -- either it's silent, the mic path "
                  "isn't wired/populated, or (SMD board) the ADC mux is on the other line.")

        print(f"\n$ loopback {args.line} 1   (should hear your own voice through the handset earpiece)")
        for line in console.send(f"loopback {args.line} 1"):
            print(" ", line)
        print("Listening for 5s, press Ctrl+C to stop early once you've confirmed it...")
        try:
            time.sleep(5.0)
        except KeyboardInterrupt:
            pass
        console.send(f"loopback {args.line} 0")
        print(f"$ loopback {args.line} 0   (off)")

        print("\nManual check required: did you hear the 1 kHz tone, and your own voice in loopback?")
        return 0


if __name__ == "__main__":
    sys.exit(main())
