#!/usr/bin/env python3
"""Bring-up: the v6 board's own parts.

Scans the I2C bus and runs the firmware's `v6check`: the ES8388 codec (0x10)
and the TCA9534A expander U6 (0x38) answer, U6 takes its settings, line 1's
SLIC (U3) is fitted or not as Settings -> Hardware says, and a coin board on
J11 (0x39, 0x48) answers if one is connected. With --j10 (or when you say so
at the prompt) it also drives J10's P4-P7 high and low one at a time and reads
them back, which finds a pin shorted to ground, to 3.3 V or to its neighbour.

Any other board type: SKIP (exit 0), so bringup_checklist.py can always run it.
"""

from __future__ import annotations
import argparse
import sys

from phone_serial import PhoneConsole, add_common_args, pick_port


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--j10", action="store_true",
                        help="run the J10 walking test without asking")
    parser.add_argument("--no-j10", action="store_true",
                        help="skip the J10 walking test without asking")
    args = parser.parse_args()

    port = pick_port(args.port)
    print(f"Using port: {port}")

    with PhoneConsole(port, args.baud) as console:
        info = console.send("board_info")
        pcb = next((l.split(":", 1)[1].strip() for l in info if l.startswith("pcb:")), "?")
        if pcb.lower() != "smd6":
            print(f"SKIP: board is '{pcb}', not v6 (smd6)")
            return 0

        print("\n$ i2c_scan")
        for line in console.send("i2c_scan"):
            print(" ", line)

        j10 = args.j10
        if not j10 and not args.no_j10:
            ans = input("\nTest J10's P4-P7? Each pin goes high for a moment, so anything "
                        "wired to J10 (a relay...) will switch. [y/N] ").strip().lower()
            j10 = ans.startswith("y")

        cmd = "v6check j10" if j10 else "v6check"
        print(f"\n$ {cmd}")
        out = console.send(cmd, timeout_s=10)
        for line in out:
            print(" ", line)

        fails = [l for l in out if l.startswith("FAIL")]
        warns = [l for l in out if l.startswith("WARN")]
        if not any(l.startswith("v6check:") for l in out):
            print("\nFAIL: no answer from v6check (firmware older than this script?)")
            return 1
        if fails:
            print(f"\nFAIL: {len(fails)} check(s) failed")
            return 1
        print(f"\nPASS{' (with warnings)' if warns else ''}: v6 parts answer.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
