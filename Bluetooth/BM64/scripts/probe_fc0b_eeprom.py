"""
FC0B with 4-byte params returns ~150 bytes!
Now try different param combos to find EEPROM data.

Known EEPROM values:
  addr 0x0000-0x0005: BT addr 40 BC 32 D0 B7 44
  addr 0x0094: 0x73 (headphone DAC)
  addr 0x0171: 0x46

Also: FC0B 1-byte returned interesting data. Scan all 256 registers.
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

s = serial.Serial(PORT, BAUD, timeout=3)
time.sleep(2)
s.reset_input_buffer()

def hci_cmd(opcode, params=b'', wait=0.5):
    cmd = bytes([0x01, opcode & 0xFF, (opcode >> 8) & 0xFF, len(params)]) + params
    s.reset_input_buffer()
    s.write(cmd)
    time.sleep(wait)
    return s.read(s.in_waiting or 200)

def fmt(d):
    return ' '.join(f'{b:02X}' for b in d) if d else 'EMPTY'

# Init
print("Init...")
hci_cmd(0x0C03, wait=1.5)
s.read(s.in_waiting)
hci_cmd(0xFC20, wait=0.5)
hci_cmd(0xFC62, bytes([0x01, 0x01, 0x01, 0x04]), wait=0.3)

# === FC0B 4-byte: try reading from addr 0x0000 ===
print("\n=== FC0B 4-byte with addr=0x0000 ===")
for t in range(4):
    r = hci_cmd(0xFC0B, bytes([t, 0x00, 0x00, 0x00]))
    if r and len(r) > 10:
        data = r[7:]  # after status
        btaddr_found = b'\x40\xBC\x32' in r
        print(f"  type={t} addr=0000: {len(r)} bytes total, BT_ADDR={'YES!' if btaddr_found else 'no'}")
        print(f"    First 48: {fmt(data[:48])}")
    else:
        print(f"  type={t} addr=0000: {fmt(r)}")

# Try larger type values
for t in [0x04, 0x08, 0x10, 0x80, 0xFF]:
    r = hci_cmd(0xFC0B, bytes([t, 0x00, 0x00, 0x00]))
    if r and len(r) > 10:
        btaddr_found = b'\x40\xBC\x32' in r
        print(f"  type={t:02X} addr=0000: {len(r)} bytes, BT_ADDR={'YES!' if btaddr_found else 'no'}")
    elif r:
        print(f"  type={t:02X}: {fmt(r)}")

# === FC0B 1-byte: scan all 256 registers looking for EEPROM values ===
print("\n=== FC0B 1-byte register scan (looking for 0x73, 0x46, 0x4A, BT addr bytes) ===")
reg_data = []
for reg in range(256):
    r = hci_cmd(0xFC0B, bytes([reg]), wait=0.15)
    if r and len(r) >= 9:
        val = r[8]  # the second data byte (first is always 01)
        reg_data.append((reg, val))
    else:
        reg_data.append((reg, None))

# Print all non-zero register values
print("\nNon-zero registers:")
for reg, val in reg_data:
    if val and val != 0:
        print(f"  reg[{reg:02X}] = 0x{val:02X} ({val:3d})", end="")
        if val == 0x73: print(" *** MATCH 0x73 (headphone DAC)", end="")
        if val == 0x46: print(" *** MATCH 0x46", end="")
        if val == 0x4A: print(" *** MATCH 0x4A", end="")
        if val == 0x40: print(" *** BT addr byte?", end="")
        if val == 0xBC: print(" *** BT addr byte?", end="")
        print()

# === Also try FC0B with 3 byte params ===
print("\n=== FC0B 3-byte: [type, page, len] ===")
for t in range(4):
    for page in range(4):
        r = hci_cmd(0xFC0B, bytes([t, page, 0x10]), wait=0.3)
        if r and len(r) > 10:
            print(f"  type={t} page={page}: {len(r)} bytes: {fmt(r[:30])}...")
        elif r and len(r) >= 7 and r[6] == 0x00:
            print(f"  type={t} page={page}: {fmt(r)}")

# === Try FC0A (write?) with read-like params ===
print("\n=== FC0A with various params ===")
for nparams in [1, 2, 3, 4]:
    params = bytes([0x00] * nparams)
    r = hci_cmd(0xFC0A, params, wait=0.3)
    if r and len(r) > 7:
        print(f"  FC0A({nparams} bytes zeros): {fmt(r)}")

print("\nDone.")
s.close()
