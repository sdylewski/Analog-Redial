#!/usr/bin/env python3
"""Shared serial helper for talking to the bring-up console (bringup.c).

All test_*.py scripts import this instead of re-implementing port discovery
and command/response framing. Requires pyserial (see requirements.txt).
"""

from __future__ import annotations
import glob
import sys
import time
from dataclasses import dataclass, field

try:
    import serial
except ImportError:
    print("pyserial is required: pip install -r requirements.txt", file=sys.stderr)
    raise

DEFAULT_BAUD = 115200
PROMPT = "phone>"

PORT_GLOBS = ["/dev/cu.usbserial*", "/dev/cu.usbmodem*", "/dev/tty.usb*", "/dev/ttyUSB*", "/dev/ttyACM*"]


def find_ports() -> list[str]:
    ports: list[str] = []
    for pattern in PORT_GLOBS:
        ports.extend(glob.glob(pattern))
    return sorted(set(ports))


def pick_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = find_ports()
    if not ports:
        raise SystemExit("No serial port found. Plug in the board or pass --port explicitly.")
    if len(ports) > 1:
        raise SystemExit(
            "Multiple serial ports found, pass one explicitly with --port:\n  "
            + "\n  ".join(ports)
        )
    return ports[0]


@dataclass
class PhoneConsole:
    port: str
    baud: int = DEFAULT_BAUD
    timeout_s: float = 5.0
    _ser: "serial.Serial | None" = field(default=None, init=False, repr=False)

    def __enter__(self) -> "PhoneConsole":
        self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
        time.sleep(0.3)  # let the console settle after DTR/RTS toggling on open
        self._ser.reset_input_buffer()
        return self

    def __exit__(self, *exc):
        if self._ser:
            self._ser.close()

    def send(self, command: str, timeout_s: float | None = None) -> list[str]:
        """Send one console command, return the lines printed before the next prompt."""
        assert self._ser is not None, "use PhoneConsole as a context manager"
        self._ser.reset_input_buffer()
        self._ser.write((command + "\n").encode("utf-8"))
        deadline = time.time() + (timeout_s if timeout_s is not None else self.timeout_s)
        lines: list[str] = []
        buf = ""
        while time.time() < deadline:
            chunk = self._ser.read(256).decode("utf-8", errors="replace")
            if not chunk:
                continue
            buf += chunk
            if PROMPT in buf:
                break
        for line in buf.splitlines():
            stripped = line.strip("\r")
            if stripped and stripped != command and PROMPT not in stripped:
                lines.append(stripped)
        return lines

    def wait_for_boot(self, timeout_s: float = 15.0) -> bool:
        """Read until the first console prompt appears (board just reset/powered up)."""
        assert self._ser is not None
        deadline = time.time() + timeout_s
        buf = ""
        while time.time() < deadline:
            chunk = self._ser.read(256).decode("utf-8", errors="replace")
            buf += chunk
            if PROMPT in buf:
                return True
        return False


def add_common_args(parser) -> None:
    parser.add_argument("--port", help="Serial port (default: auto-detect if exactly one is present)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
