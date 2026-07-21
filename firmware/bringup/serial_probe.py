#!/usr/bin/env python3
"""List serial ports that look like the ESP32-C6 console (macOS/Linux)."""

from __future__ import annotations
import sys

from phone_serial import find_ports


def main() -> int:
    ports = find_ports()
    if not ports:
        print("No serial ports found")
        return 1
    for port in ports:
        print(port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
