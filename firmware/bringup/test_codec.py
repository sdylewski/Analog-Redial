#!/usr/bin/env python3
"""Bring-up: the ES8388 audio codec.

Scans the I2C bus, checks the codec answers, initializes it, and dumps its
registers so you can eyeball that init actually wrote something sane.
"""

from __future__ import annotations
import argparse
import sys

from phone_serial import PhoneConsole, add_common_args, pick_port


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    args = parser.parse_args()

    port = pick_port(args.port)
    print(f"Using port: {port}")

    with PhoneConsole(port, args.baud) as console:
        print("\n$ i2c_scan")
        scan = console.send("i2c_scan")
        for line in scan:
            print(" ", line)
        found_codec = any("<- codec" in l for l in scan)

        print("\n$ codec_detect")
        for line in console.send("codec_detect"):
            print(" ", line)

        print("\n$ codec_init")
        init = console.send("codec_init")
        for line in init:
            print(" ", line)
        init_ok = any("ESP_OK" in l for l in init)

        print("\n$ codec_dump")
        for line in console.send("codec_dump"):
            print(" ", line)

        if not found_codec:
            print("\nFAIL: no codec ACKed on the I2C bus (expected address not found in i2c_scan)")
            return 1
        if not init_ok:
            print("\nFAIL: codec_init did not report ESP_OK")
            return 1

        print("\nPASS: codec found and initialized.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
