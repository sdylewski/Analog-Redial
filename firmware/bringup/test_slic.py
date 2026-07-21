#!/usr/bin/env python3
"""Bring-up: SLIC line interface (power-down, hook detect, ringing).

Drives one line's AG1170/AG1171 through power-up, a hook-state monitor
(lift/replace the handset while it runs), and a short ring burst. Requires a
phone or a tip/ring load plugged into the jack for the line under test.
"""

from __future__ import annotations
import argparse
import sys

from phone_serial import PhoneConsole, add_common_args, pick_port


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--line", type=int, default=0, choices=[0, 1])
    parser.add_argument("--hookmon-secs", type=int, default=8)
    parser.add_argument("--ring-secs", type=int, default=3)
    parser.add_argument("--skip-ring", action="store_true", help="skip the ring test (e.g. no bell/REN load)")
    args = parser.parse_args()

    port = pick_port(args.port)
    print(f"Using port: {port}")

    with PhoneConsole(port, args.baud) as console:
        print(f"\n$ slic {args.line} pd 1   (power up the SLIC)")
        for line in console.send(f"slic {args.line} pd 1"):
            print(" ", line)

        print(f"\n$ hookmon {args.hookmon_secs}   (lift and replace the handset on line {args.line} now)")
        hook_lines = console.send(f"hookmon {args.hookmon_secs}", timeout_s=args.hookmon_secs + 3.0)
        for line in hook_lines:
            print(" ", line)
        transitions = [l for l in hook_lines if "raw SHK" in l]
        if not transitions:
            print("WARNING: no SHK transitions logged -- either the handset was never lifted, "
                  "or the divider/GPIO for this line isn't wired as expected.")
        else:
            print(f"Saw {len(transitions)} hook-state change(s) -- looks alive.")

        if not args.skip_ring:
            print(f"\n$ ring {args.line} {args.ring_secs}   (phone should ring)")
            for line in console.send(f"ring {args.line} {args.ring_secs}", timeout_s=args.ring_secs + 3.0):
                print(" ", line)

        print("\n$ status")
        for line in console.send("status"):
            print(" ", line)

        print("\nManual check required: did the hook state track lifting/replacing the handset, "
              "and (if not skipped) did it ring?")
        return 0


if __name__ == "__main__":
    sys.exit(main())
