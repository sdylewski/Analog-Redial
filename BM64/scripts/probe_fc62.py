"""
BM64 probe — FC62 as memory type selector + FC09 read.

Working commands: FC20 (identify), FC62 (config), FC31 (setup), FC09 (read)
Theory: FC62 params select memory type, then FC09 reads from it.

The isupdate dropdown has: flash(0?), eeprom(1?), eMCU(2?)
FC62 connect params were: 01 01 01 04

EEPROM at addr 0x0000 should start with BT address: 40 BC 32 D0 B7 44
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200
BT_ADDR = b'\x40\xBC\x32\xD0\xB7\x44'

def open_port():
    s = serial.Serial(PORT, BAUD, timeout=3)
    time.sleep(2)
    s.reset_input_buffer()
    return s

def hci_cmd(s, opcode, params=b'', wait=0.5):
    cmd = bytes([0x01, opcode & 0xFF, (opcode >> 8) & 0xFF, len(params)]) + params
    s.reset_input_buffer()
    s.write(cmd)
    time.sleep(wait)
    return s.read(s.in_waiting or 200)

def fmt(d):
    return ' '.join(f'{b:02X}' for b in d) if d else 'EMPTY'

def check_eeprom(resp):
    return BT_ADDR[:4] in resp

s = open_port()

# Init sequence
print("Init: HCI_Reset + FC20 + FC62(default)")
hci_cmd(s, 0x0C03, wait=1.5)
s.read(s.in_waiting)
r = hci_cmd(s, 0xFC20, wait=1.0)
print(f"  FC20: {fmt(r)}")
hci_cmd(s, 0xFC62, bytes([0x01, 0x01, 0x01, 0x04]), wait=0.5)

# Baseline: FC09 read with default FC62 config
print("\nBaseline FC09 read (flash default):")
r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]))
print(f"  {fmt(r)}")

# === Try different FC62 params, then FC09 read ===
print("\n=== FC62 param variations + FC09 read ===")
print("  Looking for BT addr: 40 BC 32 D0 B7 44\n")

# Strategy: vary each byte of the 4-byte FC62 params
# Original: 01 01 01 04

# Vary byte 0 (might be memory type)
for b0 in range(8):
    hci_cmd(s, 0xFC62, bytes([b0, 0x01, 0x01, 0x04]), wait=0.3)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
    hit = " *** EEPROM!" if check_eeprom(r) else ""
    has_data = r and b'\x55\x55\x55\x55' not in r and len(r) > 10
    if hit or has_data:
        print(f"  FC62({b0},1,1,4) -> FC09: {fmt(r)}{hit}")

# Vary byte 1
for b1 in range(8):
    hci_cmd(s, 0xFC62, bytes([0x01, b1, 0x01, 0x04]), wait=0.3)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
    hit = " *** EEPROM!" if check_eeprom(r) else ""
    has_data = r and b'\x55\x55\x55\x55' not in r and len(r) > 10
    if hit or has_data:
        print(f"  FC62(1,{b1},1,4) -> FC09: {fmt(r)}{hit}")

# Vary byte 2 (most likely candidate for memory type)
for b2 in range(8):
    hci_cmd(s, 0xFC62, bytes([0x01, 0x01, b2, 0x04]), wait=0.3)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
    hit = " *** EEPROM!" if check_eeprom(r) else ""
    has_data = r and b'\x55\x55\x55\x55' not in r and len(r) > 10
    if hit or has_data:
        print(f"  FC62(1,1,{b2},4) -> FC09: {fmt(r)}{hit}")

# Vary byte 3
for b3 in range(8):
    hci_cmd(s, 0xFC62, bytes([0x01, 0x01, 0x01, b3]), wait=0.3)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
    hit = " *** EEPROM!" if check_eeprom(r) else ""
    has_data = r and b'\x55\x55\x55\x55' not in r and len(r) > 10
    if hit or has_data:
        print(f"  FC62(1,1,1,{b3}) -> FC09: {fmt(r)}{hit}")

# === Try FC0B as type selector (revisit with proper init) ===
print("\n=== FC0B type select (with proper FC20/FC62 init) ===")
for t in range(8):
    hci_cmd(s, 0xFC0B, bytes([t]), wait=0.2)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
    hit = " *** EEPROM!" if check_eeprom(r) else ""
    has_data = r and b'\x55\x55\x55\x55' not in r and len(r) > 10
    if hit or has_data:
        print(f"  FC0B({t}) -> FC09: {fmt(r)}{hit}")

# Try FC0B with 2 bytes
for t in range(4):
    for s2 in range(4):
        hci_cmd(s, 0xFC0B, bytes([t, s2]), wait=0.2)
        r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
        hit = " *** EEPROM!" if check_eeprom(r) else ""
        has_data = r and b'\x55\x55\x55\x55' not in r and len(r) > 10
        if hit or has_data:
            print(f"  FC0B({t},{s2}) -> FC09: {fmt(r)}{hit}")

# === Try FC31 variations as type selector ===
print("\n=== FC31 variations + FC09 read ===")
# FC31 original: 00 02 00 00 00 00 00 00 00 02 00 00
# Try changing first byte (possible memory type)
for t in range(4):
    params = bytes([t, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00])
    hci_cmd(s, 0xFC31, params, wait=0.3)
    r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.3)
    hit = " *** EEPROM!" if check_eeprom(r) else ""
    has_data = r and b'\x55\x55\x55\x55' not in r and len(r) > 10
    if hit or has_data:
        print(f"  FC31(type={t}) -> FC09: {fmt(r)}{hit}")

print("\nDone.")
s.close()
