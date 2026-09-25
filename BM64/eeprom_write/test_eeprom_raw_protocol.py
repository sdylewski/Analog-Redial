"""
Test hypotheses about BM64's RAW (non-HCI) EEPROM protocol discovered via
DMS log reverse-engineering (see eeprom_write.md, "DMS Log Reverse-Engineering
Findings" section).

Confirmed from log analysis: isUpdate writes EEPROM using packets shaped like:
    [04 00 00 00 00 00][122 bytes of EEPROM data starting at addr 0x0006]
This does NOT look like standard HCI framing (`01 xx FC ...`) — it's a
separate raw protocol. The header's exact field meaning (CMD/ADDR/LEN) is
UNCONFIRMED since only one address (block 0) was captured.

This script only sends READ-style guesses (safe, non-destructive) to try to
find the matching read command and confirm/refute the header hypothesis:
    header = [CMD:1][RESERVED:1][ADDR_LO:1][ADDR_HI:1][RESERVED:1][RESERVED:1]

Cross-platform (Windows/macOS/Linux) via pyserial. Auto-lists ports if none
given.

Usage:
    python test_eeprom_raw_protocol.py [PORT]

Requires:
    - ESP32-C6 bridge sketch uploaded with BOOT_MODE 0 (IBDK/programming mode)
    - Arduino IDE / Serial Monitor fully closed (port must be free)
    - pip install pyserial
"""
import serial
import serial.tools.list_ports
import time
import sys

BAUD = 115200

KNOWN_EEPROM_BYTES = {
    0x0094: 0x73,   # headphone DAC enable
    0x0171: 0x46,   # DAC output path routing
    0x01FC: 0x4A,   # headphone feature flags
    0x0006: 0x01,   # first byte of the confirmed write payload
    0x0007: 0x04,
    0x0008: 0x04,
    0x0009: 0x24,
}


def pick_port(argv_port=None):
    if argv_port:
        return argv_port
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found. Plug in the ESP32-C6 and retry.")
        sys.exit(1)
    print("Available serial ports:")
    for i, p in enumerate(ports):
        print(f"  [{i}] {p.device}  {p.description}")
    if len(ports) == 1:
        print(f"\nAuto-selecting only available port: {ports[0].device}")
        return ports[0].device
    idx = input("Select port index: ").strip()
    return ports[int(idx)].device


def open_port(port):
    try:
        s = serial.Serial(port, BAUD, timeout=2)
        time.sleep(2)  # let ESP32 boot / enter IBDK mode
        s.reset_input_buffer()
        return s
    except serial.SerialException as e:
        print(f"FAILED to open {port}: {e}")
        print("Make sure Arduino IDE / Serial Monitor is fully closed.")
        sys.exit(1)


def format_hex(data):
    return ' '.join(f'{b:02X}' for b in data) if data else 'EMPTY'


def send_raw(s, payload, read_len=256, settle=0.3):
    s.reset_input_buffer()
    s.write(payload)
    time.sleep(settle)
    return s.read(s.in_waiting or read_len)


def check_against_known(resp):
    """See if any KNOWN_EEPROM_BYTES value appears at a plausible spot."""
    hits = []
    for addr, val in KNOWN_EEPROM_BYTES.items():
        if val in resp:
            hits.append((addr, val, resp.index(val)))
    return hits


def main():
    port = pick_port(sys.argv[1] if len(sys.argv) > 1 else None)
    print(f"\nOpening {port} @ {BAUD}...")
    s = open_port(port)

    print("\n" + "=" * 70)
    print("Test 1: Confirmed WRITE header replayed with different CMD bytes")
    print("=" * 70)
    print("Hypothesis: header = [CMD][RESERVED][ADDR_LO][ADDR_HI][RESERVED][RESERVED]")
    print("Known WRITE cmd = 0x04. Trying other CMD values as potential READ.\n")

    # addr = 0x0000 (block 0), same block the write targeted
    for cmd in range(0x00, 0x10):
        if cmd == 0x04:
            continue  # known write, skip to avoid accidental write
        header = bytes([cmd, 0x00, 0x00, 0x00, 0x00, 0x00])
        resp = send_raw(s, header)
        if resp:
            print(f"  CMD=0x{cmd:02X} -> {format_hex(resp[:32])}"
                  f"{' ...' if len(resp) > 32 else ''}  ({len(resp)} bytes)")
            hits = check_against_known(resp)
            for addr, val, idx in hits:
                print(f"      *** possible match: byte 0x{val:02X} "
                      f"(matches known EEPROM[0x{addr:04X}]) at response offset {idx}")

    print("\n" + "=" * 70)
    print("Test 2: Try CMD=0x04 (write opcode) but with a READ-length param")
    print("        guess in place of reserved bytes, targeting addr 0x0094")
    print("=" * 70)
    # If reserved bytes double as length, try requesting 16 bytes at 0x0094
    addr = 0x0094
    addr_lo = addr & 0xFF
    addr_hi = (addr >> 8) & 0xFF
    variants = {
        "cmd03_addr_len16":      bytes([0x03, 0x00, addr_lo, addr_hi, 0x10, 0x00]),
        "cmd05_addr_len16":      bytes([0x05, 0x00, addr_lo, addr_hi, 0x10, 0x00]),
        "cmd04_addr_len16":      bytes([0x04, 0x00, addr_lo, addr_hi, 0x10, 0x00]),
        "cmd02_addr_len16":      bytes([0x02, 0x00, addr_lo, addr_hi, 0x10, 0x00]),
        "cmd01_addr_len16":      bytes([0x01, 0x00, addr_lo, addr_hi, 0x10, 0x00]),
        "cmd00_addr_len16":      bytes([0x00, 0x00, addr_lo, addr_hi, 0x10, 0x00]),
    }
    for name, payload in variants.items():
        resp = send_raw(s, payload)
        if resp:
            print(f"  {name}: sent={format_hex(payload)}")
            print(f"      -> {format_hex(resp[:32])}{' ...' if len(resp) > 32 else ''}")
            if 0x73 in resp:
                print(f"      *** 0x73 FOUND in response (matches EEPROM[0x0094]) ***")

    print("\n" + "=" * 70)
    print("Test 3: Raw sweep — try CMD bytes 0x00-0x1F with addr=0x0094,")
    print("        6-byte header format, looking for 0x73 in the reply")
    print("=" * 70)
    for cmd in range(0x00, 0x20):
        header = bytes([cmd, 0x00, addr_lo, addr_hi, 0x00, 0x00])
        resp = send_raw(s, header)
        if resp and 0x73 in resp:
            print(f"  *** CMD=0x{cmd:02X} -> {format_hex(resp)}  (contains 0x73!)")

    print("\nDone. Closing port.")
    s.close()


if __name__ == '__main__':
    main()
