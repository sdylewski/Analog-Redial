#!/usr/bin/env python3
"""List serial ports that look like the ESP32-C6 console (macOS, Linux, Windows)."""

from __future__ import annotations
import sys

from phone_serial import find_board_ports, find_ports


def main() -> int:
    boards = find_board_ports()
    if boards:
        print("ESP32-C6 boards (native USB):")
        for port in boards:
            print("  " + port)
        return 0
    ports = find_ports()
    if not ports:
        print("No serial ports found. Check the cable carries data and is in the "
              "DevKit's USB port, not UART.")
        return 1
    print("No ESP32-C6 found; other serial ports:")
    for port in ports:
        print("  " + port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
