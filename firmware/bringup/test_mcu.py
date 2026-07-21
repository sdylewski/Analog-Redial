#!/usr/bin/env python3
"""Bring-up: MCU / UART console.

Confirms the ESP32-C6 boots, the bring-up console REPL responds, and reports
which board type + pin map the running firmware was built for. Run this
first -- every other test_*.py script depends on the console being alive.
"""

from __future__ import annotations
import argparse
import sys

from phone_serial import PhoneConsole, add_common_args, pick_port


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--wait-boot", action="store_true",
                         help="wait for a fresh boot banner instead of assuming the console is already up")
    args = parser.parse_args()

    port = pick_port(args.port)
    print(f"Using port: {port}")

    with PhoneConsole(port, args.baud) as console:
        if args.wait_boot:
            print("Waiting for boot banner (reset the board now if needed)...")
            if not console.wait_for_boot():
                print("FAIL: no console prompt seen within timeout")
                return 1

        lines = console.send("board_info")
        if not lines:
            print("FAIL: no response to 'board_info' -- is the board flashed and the port right?")
            return 1

        for line in lines:
            print(" ", line)

        board_line = next((l for l in lines if l.startswith("board:")), None)
        if board_line is None:
            print("FAIL: unexpected response shape")
            return 1

        print(f"PASS: console alive, {board_line}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
