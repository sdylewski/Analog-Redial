#!/usr/bin/env python3
"""Work out what board is plugged in, build the matching firmware, flash it.

  1. Board type -- from --board, or else read from the firmware already on
     the board (its `board_info` reports the exact PCB and the type this
     board last confirmed booting as), or else asked.
  2. Flash size -- read from the chip with esptool (8 MB or 16 MB); no
     firmware needed.
  3. Build directory -- picked from those two, and created with the right
     defaults if it doesn't exist yet:
       SMD4, 16 MB   build/            (the tracked sdkconfig)
       other, 16 MB  build.<type>/
       any, 8 MB     build.<type>_8mb/ (partitions_8mb.csv: smaller
                                        recordings area, same OTA slots
                                        and bundled audio)
  4. Build, flash, and check the board comes back up as the right PCB.
  5. Optionally (--setup) run first_setup.py.

Run it from an ESP-IDF shell (`. ~/esp/esp-idf/export.sh`), with no
`idf.py monitor` holding the port.

  ./flash_board.py                       # detect everything, ask before flashing
  ./flash_board.py --board smd2 --yes
  ./flash_board.py --setup --host telephono.local
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent
sys.path.insert(0, str(HERE))

# board key -> (sdkconfig overlay, what board_info's "pcb:" line says)
BOARDS = {
    "smd2":       ("sdkconfig.smd2.defaults",       "SMD2"),
    "smd4":       ("sdkconfig.smd4.defaults",       "SMD4"),
    "smd5":       ("sdkconfig.smd5.defaults",       "SMD5"),
    "smd5_cheap": ("sdkconfig.smd5_cheap.defaults", "SMD5 (clone module)"),
}
PCB_TO_BOARD = {pcb: key for key, (_, pcb) in BOARDS.items()}


def usb_ports() -> list[str]:
    from phone_serial import find_board_ports
    return find_board_ports()


def read_board_info(port: str, boot_wait: float = 3) -> dict:
    """board_info from whatever firmware is running now; {} if none answers."""
    from phone_serial import PhoneConsole
    try:
        with PhoneConsole(port) as con:
            if not (con.is_alive(boot_wait) or con.wait_for_boot(timeout_s=boot_wait)):
                return {}
            info = {}
            for line in con.send("board_info", timeout_s=5):
                if ":" in line:
                    k, v = line.split(":", 1)
                    info[k.strip()] = v.strip()
            return info
    except Exception as e:  # port busy, not our firmware, ...
        print(f"  (couldn't read the running firmware: {e})")
        return {}


def detect_board(info: dict) -> tuple[str | None, str]:
    """(board key or None, how we know)."""
    recorded, pcb = info.get("pcb_recorded", "-"), info.get("pcb", "")
    if recorded in PCB_TO_BOARD:
        return PCB_TO_BOARD[recorded], f"the board last confirmed booting as {recorded}"
    if pcb in PCB_TO_BOARD:
        return PCB_TO_BOARD[pcb], f"the firmware on it was built for {pcb}"
    if info.get("board") == "tht":
        return None, "it's running through-hole (THT) firmware"
    if info.get("board") == "smd":
        return None, "its firmware is too old to say which SMD board"
    return None, "no firmware answered on the console"


def read_flash_size(port: str) -> str | None:
    proc = subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32c6", "-p", port,
                           "flash_id"], capture_output=True, text=True)
    m = re.search(r"Detected flash size: (\S+)", proc.stdout + proc.stderr)
    return m.group(1) if m else None


def build_dir_for(board: str, flash: str) -> tuple[str, list[str]]:
    """(build dir, SDKCONFIG_DEFAULTS chain) -- chain empty for the default build/."""
    if board == "smd4" and flash == "16MB":
        return "build", []
    chain = ["sdkconfig.defaults", BOARDS[board][0]]
    if flash == "8MB":
        chain.append("sdkconfig.8mb.defaults")
        return f"build.{board}_8mb", chain
    return f"build.{board}", chain


def idf(*args: str) -> int:
    print("  $ idf.py " + " ".join(args))
    return subprocess.call(["idf.py", *args], cwd=FIRMWARE)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="Serial port (default: the one board plugged in)")
    ap.add_argument("--board", choices=sorted(BOARDS), help="Skip detection")
    ap.add_argument("--flash", choices=("8MB", "16MB"), help="Skip the esptool flash-size read")
    ap.add_argument("--yes", action="store_true", help="Don't ask before flashing")
    ap.add_argument("--dry-run", action="store_true", help="Detect and print the plan only")
    ap.add_argument("--setup", action="store_true", help="Run first_setup.py afterwards")
    ap.add_argument("--host", help="Passed to first_setup.py with --setup")
    args = ap.parse_args()

    if not shutil.which("idf.py"):
        ap.error("idf.py not found -- run `. ~/esp/esp-idf/export.sh` first")

    ports = usb_ports()
    port = args.port or (ports[0] if len(ports) == 1 else None)
    if not port:
        ap.error("pass --port: " + ("no board found" if not ports else
                                     "several boards plugged in: " + ", ".join(ports)))
    print(f"Port: {port}")

    # 1. board type (console first -- esptool below resets the chip)
    board = args.board
    if board:
        print(f"Board: {board} (from --board)")
    else:
        info = read_board_info(port)
        board, why = detect_board(info)
        if board:
            print(f"Board: {board} -- {why}")
        else:
            print(f"Board: unknown -- {why}")
            if args.yes or not sys.stdin.isatty():
                ap.error("pass --board " + "|".join(sorted(BOARDS)))
            while board not in BOARDS:
                board = input(f"Which board is this? [{'/'.join(sorted(BOARDS))}] ").strip().lower()

    # 2. flash size
    flash = args.flash
    if flash:
        print(f"Flash: {flash} (from --flash)")
    else:
        flash = read_flash_size(port)
        if not flash:
            print("esptool couldn't read the chip -- port busy (monitor open?) or the board "
                  "is stuck; re-plug USB and retry, or pass --flash.")
            return 1
        print(f"Flash: {flash} (read from the chip)")
        if flash not in ("8MB", "16MB"):
            print(f"No layout for {flash} flash -- only 8 MB and 16 MB are supported.")
            return 1

    # 3. build directory
    bdir, chain = build_dir_for(board, flash)
    print(f"Build: {bdir}/")
    if args.dry_run:
        return 0
    if not args.yes:
        if input(f"Build and flash {board} / {flash} to {port}? [y/N] ").strip().lower() != "y":
            return 1

    sdk = [] if not chain else ["-D", f"SDKCONFIG={bdir}/sdkconfig"]
    if chain and not (FIRMWARE / bdir / "sdkconfig").exists():
        print(f"Creating {bdir}/ from {';'.join(chain)}")
        if idf("-B", bdir, *sdk, "-D", f"SDKCONFIG_DEFAULTS={';'.join(chain)}",
               "set-target", "esp32c6") != 0:
            return 1

    # 4. build + flash
    if idf("-B", bdir, *sdk, "build") != 0:
        return 1
    if idf("-B", bdir, *sdk, "-p", port, "flash") != 0:
        print("Flash failed. If it's stuck in download mode, retry once; if that fails, "
              "unplug and re-plug USB.")
        return 1

    print("Waiting for the board to boot...")
    time.sleep(2)
    info = read_board_info(port, boot_wait=20)
    pcb = info.get("pcb", "")
    if pcb != BOARDS[board][1]:
        print(f"WARN: board_info after flashing says pcb='{pcb or '?'}', "
              f"expected {BOARDS[board][1]}")
    else:
        print(f"OK: running {pcb} firmware")
    recorded = info.get("pcb_recorded", "-")
    if recorded not in ("-", pcb):
        print(f"WARN: this board previously confirmed booting as {recorded} -- if that was "
              f"right, this is the wrong firmware. If {pcb} is right, confirm it in "
              "Settings -> Hardware.")

    # 5. setup
    if args.setup:
        cmd = [sys.executable, str(HERE / "first_setup.py"), "--port", port]
        if args.host:
            cmd += ["--host", args.host]
        return subprocess.call(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
