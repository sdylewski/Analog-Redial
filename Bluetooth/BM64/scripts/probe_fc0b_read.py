"""
Test if FC0B is actually the EEPROM read command.
Also test FC09 at addr 0x0094 to confirm it's NOT reading EEPROM.

EEPROM addr 0x0094 should contain 0x73 (headphone DAC flag we wrote).
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

# === Test FC09 at EEPROM key addresses ===
print("\n=== FC09 at EEPROM addresses (should NOT be EEPROM values) ===")
for addr, expected_eeprom in [(0x0000, "BT addr"), (0x0094, "0x73"), (0x0171, "0x46"), (0x01FC, "0x4A")]:
    r = hci_cmd(0xFC09, bytes([addr & 0xFF, (addr >> 8) & 0xFF, 0x04]))
    print(f"  FC09 addr={addr:04X}: {fmt(r)}")
    if r and len(r) > 10:
        data = r[10:]  # skip header+status+addr_echo+len_echo
        print(f"    Data: {fmt(data)} (EEPROM expected: {expected_eeprom})")

# === Test FC0B with various param formats ===
print("\n=== FC0B — testing if it reads EEPROM ===")

# Try 1-byte params
print("\nFC0B 1-byte params:")
for p in [0x00, 0x01, 0x02, 0x94]:
    r = hci_cmd(0xFC0B, bytes([p]))
    print(f"  FC0B({p:02X}): {fmt(r)}")

# Try 2-byte params (addr_lo, addr_hi)
print("\nFC0B 2-byte params [addr]:")
for addr in [0x0000, 0x0001, 0x0094, 0x0171, 0x01FC]:
    r = hci_cmd(0xFC0B, bytes([addr & 0xFF, (addr >> 8) & 0xFF]))
    print(f"  FC0B addr={addr:04X}: {fmt(r)}")

# Try 3-byte params [addr, len]
print("\nFC0B 3-byte params [addr_lo, addr_hi, len]:")
for addr, length in [(0x0000, 0x10), (0x0094, 0x01), (0x0094, 0x04)]:
    r = hci_cmd(0xFC0B, bytes([addr & 0xFF, (addr >> 8) & 0xFF, length]))
    print(f"  FC0B addr={addr:04X} len={length}: {fmt(r)}")

# Try 4-byte params [type, addr_lo, addr_hi, len]
print("\nFC0B 4-byte params [type, addr, len]:")
for t in [0x00, 0x01, 0x02, 0x03]:
    r = hci_cmd(0xFC0B, bytes([t, 0x94, 0x00, 0x01]))
    print(f"  FC0B type={t} addr=0094 len=1: {fmt(r)}")

# === Try other opcodes we haven't tested ===
print("\n=== Other untested vendor opcodes ===")
# Try every opcode from FC00-FC1F with addr params
for ocf in range(0x20):
    if ocf in [0x03, 0x09, 0x0B, 0x20]:
        continue  # already tested
    r = hci_cmd(0xFC00 | ocf, bytes([0x94, 0x00, 0x01]), wait=0.3)
    if r and len(r) > 7:  # got more than just a status ack
        print(f"  FC{ocf:02X}(94,00,01): {fmt(r)}")
    elif r and len(r) >= 7 and r[6] == 0x00:  # status=0 (success)
        print(f"  FC{ocf:02X}(94,00,01): {fmt(r)} [SUCCESS]")

print("\nDone.")
s.close()
