#!/usr/bin/env python3
"""Bring-up: DTMF and call-progress tone generation.

Plays each DTMF digit and the standard call-progress tones (dial/ringback/
busy) on one line. Listen at the handset, or clip a scope/meter across tip/
ring, to confirm.
"""

from __future__ import annotations
import argparse
import sys

from phone_serial import PhoneConsole, add_common_args, pick_port


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--line", type=int, default=0, choices=[0, 1])
    parser.add_argument("--digits", default="1234567890*#")
    args = parser.parse_args()

    port = pick_port(args.port)
    print(f"Using port: {port}")

    with PhoneConsole(port, args.baud) as console:
        print(f"\n$ dtmf_gen {args.digits} {args.line}")
        for line in console.send(f"dtmf_gen {args.digits} {args.line}", timeout_s=len(args.digits) * 0.3 + 2.0):
            print(" ", line)

        for name in ("dialtone", "ringback", "busy"):
            print(f"\n$ {name} {args.line}")
            for line in console.send(f"{name} {args.line}"):
                print(" ", line)
        console.send(f"silence {args.line}")

        print("\nManual check required: did you hear each DTMF digit and all three call-progress tones?")
        return 0


if __name__ == "__main__":
    sys.exit(main())
