"""
BM64 HCI probe round 4 — targeted bank/page select for EEPROM access.

Theory: FC09 reads 16-bit addresses within a 64KB page.
FC0B (or another cmd) selects which page/bank is active.
EEPROM is likely at flash offset 0x1F0000 (bank 2 from FlashHeader_Config.ini).

We look for BT address 40 BC 32 D0 B7 44 at EEPROM offset 0x0000.

Run: python -u probe_hci4.py
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200
BT_ADDR = b'\x40\xBC\x32\xD0\xB7\x44'

def open_port():
    s = serial.Serial(PORT, BAUD, timeout=2)
    time.sleep(2)
    s.reset_input_buffer()
    return s

def hci_cmd(s, opcode, params=b'', wait=0.4):
    cmd = bytes([0x01, opcode & 0xFF, (opcode >> 8) & 0xFF, len(params)]) + params
    s.reset_input_buffer()
    s.write(cmd)
    time.sleep(wait)
    resp = s.read(s.in_waiting or 200)
    # Check for HW error flood and skip
    if resp == b'\x04\x10\x01\x10':
        return b''
    return resp

def fmt(d):
    return ' '.join(f'{b:02X}' for b in d) if d else 'EMPTY'

s = open_port()
print("HCI Reset...")
hci_cmd(s, 0x0C03, wait=1.0)
s.read(s.in_waiting)

# === Test 1: FC0B as bank select with flash bank addresses ===
print("\n=== FC0B bank select + FC09 read ===")
# Flash banks from FlashHeader_Config.ini: 0x000000, 0x1E0000, 0x1F0000
# Page numbers to try (byte 2 of the flash address)
bank_values = [0x00, 0x01, 0x0F, 0x10, 0x1E, 0x1F, 0x20, 0x30,
               0x3E, 0x3F, 0x40, 0x7F, 0x80, 0xFE, 0xFF]

for bank in bank_values:
    # Try single-byte
    hci_cmd(s, 0xFC0B, bytes([bank]), wait=0.2)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.4)
    if BT_ADDR[:4] in r:
        print(f"  *** EEPROM HIT! FC0B({bank:02X}) -> FC09 -> {fmt(r)}")
    elif r and b'\x55\x55\x55\x55' not in r and len(r) > 10:
        print(f"  FC0B({bank:02X}): FC09 -> {fmt(r)}")

# === Test 2: FC0B with 2-byte bank (little-endian page number) ===
print("\n=== FC0B 2-byte bank select ===")
for page in [0x001E, 0x001F, 0x0020, 0x001F0, 0x01F0, 0x1E00, 0x1F00]:
    lo = page & 0xFF
    hi = (page >> 8) & 0xFF
    hci_cmd(s, 0xFC0B, bytes([lo, hi]), wait=0.2)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.4)
    if BT_ADDR[:4] in r:
        print(f"  *** EEPROM HIT! FC0B({lo:02X},{hi:02X}) -> FC09 -> {fmt(r)}")
    elif r and b'\x55\x55\x55\x55' not in r and len(r) > 10:
        print(f"  FC0B({lo:02X},{hi:02X}): FC09 -> {fmt(r)}")

# === Test 3: FC0B with 3-byte params (full 24-bit address) ===
print("\n=== FC0B 3-byte params ===")
for addr in [0x1F0000, 0x1E0000, 0x000000, 0x100000, 0x0F0000]:
    a0 = addr & 0xFF
    a1 = (addr >> 8) & 0xFF
    a2 = (addr >> 16) & 0xFF
    hci_cmd(s, 0xFC0B, bytes([a0, a1, a2]), wait=0.2)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.4)
    if BT_ADDR[:4] in r:
        print(f"  *** EEPROM HIT! FC0B base=0x{addr:06X} -> FC09 -> {fmt(r)}")
    elif r and b'\x55\x55\x55\x55' not in r and len(r) > 10:
        print(f"  FC0B base=0x{addr:06X}: FC09 -> {fmt(r)}")

# === Test 4: Maybe FC09 itself takes a 3-byte or 4-byte address ===
print("\n=== FC09 with 3+ byte address (24-bit flash addr + length) ===")
for base in [0x1F0000, 0x1E0000, 0x1FF000, 0x1FE000, 0x1F8000,
             0x100000, 0x0F0000, 0x080000]:
    a0 = base & 0xFF
    a1 = (base >> 8) & 0xFF
    a2 = (base >> 16) & 0xFF
    # 4 params: addr_lo, addr_mid, addr_hi, length
    r = hci_cmd(s, 0xFC09, bytes([a0, a1, a2, 0x10]), wait=0.4)
    if BT_ADDR[:4] in r:
        print(f"  *** HIT! FC09 addr=0x{base:06X} -> {fmt(r)}")
    elif r and b'\x55\x55\x55\x55' not in r and len(r) > 10:
        data_preview = r[7:23] if len(r) > 7 else r
        if len(set(data_preview)) > 2:
            print(f"  0x{base:06X}: {fmt(r)}")

# === Test 5: FC09 with 5-byte params (type + 24-bit addr + length) ===
print("\n=== FC09 5-byte [type, addr24, length] ===")
for t in [0x00, 0x01, 0x02, 0x03]:
    for base in [0x000000, 0x1F0000, 0x1E0000]:
        a0 = base & 0xFF
        a1 = (base >> 8) & 0xFF
        a2 = (base >> 16) & 0xFF
        r = hci_cmd(s, 0xFC09, bytes([t, a0, a1, a2, 0x10]), wait=0.4)
        if BT_ADDR[:4] in r:
            print(f"  *** HIT! type={t:02X} addr=0x{base:06X} -> {fmt(r)}")
        elif r and len(r) > 15 and b'\x55\x55' not in r:
            print(f"  type={t:02X} addr=0x{base:06X}: {fmt(r)}")

# === Test 6: Try FC0A (right next to FC09/FC0B) ===
print("\n=== FC0A with various params ===")
for params in [bytes([0x00, 0x00, 0x10]),
               bytes([0x01, 0x00, 0x00, 0x10]),
               bytes([0x00, 0x00, 0x1F, 0x10])]:
    r = hci_cmd(s, 0xFC0A, params, wait=0.4)
    if BT_ADDR[:4] in r:
        print(f"  *** HIT! FC0A({fmt(params)}) -> {fmt(r)}")
    elif r and len(r) > 10:
        print(f"  FC0A({fmt(params)}) -> {fmt(r)}")

print("\nDone.")
s.close()
