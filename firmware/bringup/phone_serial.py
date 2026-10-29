#!/usr/bin/env python3
"""Shared serial helper for talking to the bring-up console (bringup.c).

All test_*.py scripts import this instead of re-implementing port discovery
and command/response framing. Requires pyserial (see requirements.txt).
"""

from __future__ import annotations
import glob
import re
import sys
import time
from dataclasses import dataclass, field

try:
    import serial
except ImportError:
    print("pyserial is required: pip install -r requirements.txt", file=sys.stderr)
    raise

DEFAULT_BAUD = 115200
# The REPL prompt is the board's own name, lower-cased, plus ">" (bringup.c
# builds it from board_name_get(), e.g. "telephono>"), so match any
# name-shaped word followed by ">" at the start of a line rather than one
# fixed string.
PROMPT_RE = re.compile(r"(?:^|\n)[A-Za-z0-9_.-]+> ?")
PROMPT_LINE_RE = re.compile(r"[A-Za-z0-9_.-]+> ?(.*)")
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def _clean(text: str) -> str:
    return ANSI_RE.sub("", text).replace("\r", "")

PORT_GLOBS = ["/dev/cu.usbserial*", "/dev/cu.usbmodem*", "/dev/tty.usb*", "/dev/ttyUSB*", "/dev/ttyACM*"]


def find_ports() -> list[str]:
    ports: list[str] = []
    for pattern in PORT_GLOBS:
        ports.extend(glob.glob(pattern))
    return sorted(set(ports))


ESPRESSIF_USB_VID = 0x303A   # the ESP32-C6's native USB (USB-Serial/JTAG)


def find_board_ports() -> list[str]:
    """Native-USB boards only, one entry per board, on macOS, Linux and
    Windows (COMn). macOS lists each one twice (cu.* and tty.*); cu.* is the
    one to use, and the only one list_ports reports."""
    try:
        from serial.tools import list_ports
        found = [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_USB_VID]
        if found:
            return sorted(set(found))
    except ImportError:
        pass
    return sorted(set(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")))


def pick_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = find_board_ports() or find_ports()
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
        # Leave DTR and RTS at pyserial's defaults (both asserted). On the
        # C6's native USB port, RTS asserted with DTR released is the chip
        # reset request -- forcing both low on open drops DTR first and
        # resets the board (seen as rst:0x15 USB_UART_HPSYS). Both asserted
        # resets nothing.
        # Retry for a while: right after a reset the C6's native USB port
        # disappears and re-enumerates, so the device node can be missing
        # or not ready for a few seconds.
        deadline = time.time() + 10
        while True:
            try:
                self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
                break
            except (serial.SerialException, OSError):
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        time.sleep(0.3)
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
            # The echoed command line comes back too; only a prompt after
            # the command's output counts as "done".
            after = _clean(buf).split(command, 1)[-1]
            if PROMPT_RE.search(after):
                break
        lines: list[str] = []
        for line in _clean(buf).splitlines():
            stripped = line.strip()
            if not stripped or stripped == command:
                continue
            echo = PROMPT_LINE_RE.fullmatch(stripped)   # "name>" or "name> <command>"
            if echo and echo.group(1) in ("", command):
                continue
            lines.append(stripped)
        return lines

    def is_alive(self, timeout_s: float = 2.0) -> bool:
        """True if the REPL answers an empty line with a prompt. Sends a
        newline every second: one sent while the board is still booting is
        simply lost, and a single try made a just-reset board look dead."""
        assert self._ser is not None
        self._ser.reset_input_buffer()
        deadline = time.time() + timeout_s
        next_nl = 0.0
        buf = ""
        while time.time() < deadline:
            if time.time() >= next_nl:
                self._ser.write(b"\n")
                next_nl = time.time() + 1.0
            buf += self._ser.read(256).decode("utf-8", errors="replace")
            if PROMPT_RE.search(_clean(buf)):
                time.sleep(0.2)                      # let extra prompts arrive,
                self._ser.reset_input_buffer()       # then drop them
                return True
        return False

    def wait_for_boot(self, timeout_s: float = 15.0) -> bool:
        """Read until the first console prompt appears (board just reset/powered up)."""
        assert self._ser is not None
        deadline = time.time() + timeout_s
        buf = ""
        while time.time() < deadline:
            chunk = self._ser.read(256).decode("utf-8", errors="replace")
            buf += chunk
            if PROMPT_RE.search(_clean(buf)):
                return True
        return False


def add_common_args(parser) -> None:
    parser.add_argument("--port", help="Serial port (default: auto-detect if exactly one is present)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
