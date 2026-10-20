"""
BM64 HCI probe round 3 — exhaustive search for EEPROM read command.

We know EEPROM at 0x0000 starts with 40 BC 32 D0 B7 44 (BT address).
Try every plausible way to read it.

Run: python -u probe_hci3.py
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200
BT_ADDR = bytes([0x40, 0xBC, 0x32, 0xD0, 0xB7, 0x44])

def open_port():
    try:
        s = serial.Serial(PORT, BAUD, timeout=2)
        time.sleep(2)
        s.reset_input_buffer()
        return s
    except serial.SerialException as e:
        print(f"FAILED: {e}")
        sys.exit(1)

def hci_cmd(s, opcode, params=b'', wait=0.5):
    cmd = bytes([0x01, opcode & 0xFF, (opcode >> 8) & 0xFF, len(params)]) + params
    s.reset_input_buffer()
    s.write(cmd)
    time.sleep(wait)
    return s.read(s.in_waiting or 200)

def fmt(data):
    return ' '.join(f'{b:02X}' for b in data) if data else 'EMPTY'

def has_eeprom_data(resp):
    """Check if response contains our BT address (EEPROM marker)."""
    return BT_ADDR[:4] in resp

s = open_port()
print("HCI Reset...")
hci_cmd(s, 0x0C03, wait=1.0)
s.read(s.in_waiting)

# === Strategy 1: FC09 with different first-byte (type selector) ===
print("\n=== FC09 with type prefix byte ===")
for t in range(0x10):
    # Format: [type, addr_lo, addr_hi, len]
    r = hci_cmd(s, 0xFC09, bytes([t, 0x00, 0x00, 0x10]))
    if has_eeprom_data(r):
        print(f"  *** HIT! FC09 type={t:02X} -> {fmt(r)}")

# === Strategy 2: FC09 with 4-byte address (EEPROM might be at high offset) ===
print("\n=== FC09 with 4-byte addr ===")
offsets = [0x000000, 0x010000, 0x020000, 0x040000, 0x080000,
           0x0F0000, 0x100000, 0x1F0000, 0x200000, 0x3F0000,
           0x400000, 0x7F0000, 0xFF0000]
for off in offsets:
    a0 = off & 0xFF
    a1 = (off >> 8) & 0xFF
    a2 = (off >> 16) & 0xFF
    r = hci_cmd(s, 0xFC09, bytes([a0, a1, a2, 0x10]))
    if has_eeprom_data(r):
        print(f"  *** HIT! FC09 addr=0x{off:06X} -> {fmt(r)}")
    elif r and b'\x55\x55\x55\x55' not in r and r != b'\x04\x10\x01\x10':
        # Different data (not 0x55, not HW error)
        data_part = r[7:] if len(r) > 7 else r
        if len(set(data_part)) > 2:  # not all same byte
            print(f"  Interesting at 0x{off:06X}: {fmt(r)}")

# === Strategy 3: Scan ALL vendor opcodes FC00-FCFF for EEPROM read ===
print("\n=== Scan FC00-FCFF with addr 0x0000 len 0x10 ===")
for ocf in range(0x100):
    opcode = 0xFC00 | ocf
    if opcode in [0xFC09, 0xFC0B, 0x0C03]:
        continue  # already tested
    r = hci_cmd(s, opcode, bytes([0x00, 0x00, 0x10]), wait=0.3)
    if has_eeprom_data(r):
        print(f"  *** HIT! FC{ocf:02X} -> {fmt(r)}")
    elif r and len(r) > 15 and r != b'\x04\x10\x01\x10':
        # Long response with actual data
        if b'\x55\x55\x55\x55' not in r:
            print(f"  FC{ocf:02X} long response: {fmt(r[:40])}")

# === Strategy 4: Maybe it's a 2-step with a different setup command ===
print("\n=== Try setup commands then FC09 read ===")
# Try sending various FC opcodes as "setup", then read
setup_opcodes = [0xFC01, 0xFC02, 0xFC03, 0xFC04, 0xFC05, 0xFC06,
                 0xFC07, 0xFC08, 0xFC0A, 0xFC0C, 0xFC0D, 0xFC0E, 0xFC0F]
for setup_op in setup_opcodes:
    for param in [bytes([0x01]), bytes([0x02]), bytes([0x01, 0x00]),
                  bytes([0x02, 0x00]), bytes([0x01, 0x01])]:
        hci_cmd(s, setup_op, param, wait=0.2)
        r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
        if has_eeprom_data(r):
            print(f"  *** HIT! Setup FC{setup_op&0xFF:02X}({fmt(param)}) then FC09 -> {fmt(r)}")
            break

# === Strategy 5: Try reading from "EEPROM" subtype address spaces ===
# isUpdate has "subtype: Serial Flash" — maybe EEPROM has its own subtype
print("\n=== FC09 read with 5-6 byte params (type+addr+len combos) ===")
for params in [
    bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x10]),  # type=1, addr32=0, len16=10
    bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x10]),  # type=2
    bytes([0x03, 0x00, 0x00, 0x00, 0x00, 0x10]),  # type=3
    bytes([0x00, 0x01, 0x00, 0x00, 0x00, 0x10]),  # subtype=1
    bytes([0x01, 0x01, 0x00, 0x00, 0x10]),         # type=1,sub=1,addr16,len
    bytes([0x02, 0x01, 0x00, 0x00, 0x10]),         # type=2,sub=1
]:
    r = hci_cmd(s, 0xFC09, params, wait=0.5)
    if has_eeprom_data(r):
        print(f"  *** HIT! FC09({fmt(params)}) -> {fmt(r)}")
    elif r and len(r) > 10 and b'\x55\x55' not in r and r != b'\x04\x10\x01\x10':
        print(f"  FC09({fmt(params)}) -> {fmt(r)}")

print("\nDone.")
s.close()
