"""
Brute-force scan of ALL vendor HCI opcodes FC00-FCFF.
For each, send with read-like params and check for:
1. Long responses (possible memory reads)
2. Responses containing known EEPROM bytes (40 BC 32 = BT addr)
3. Status=0 (command recognized)

Also try FC09 with different length values to understand its exact format.
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

s = serial.Serial(PORT, BAUD, timeout=3)
time.sleep(2)
s.reset_input_buffer()

def hci_cmd(opcode, params=b'', wait=0.4):
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
r = hci_cmd(0xFC20, wait=0.5)
hci_cmd(0xFC62, bytes([0x01, 0x01, 0x01, 0x04]), wait=0.3)

# === First, understand FC09 format exactly ===
print("\n=== FC09 format tests ===")
# 3-byte params: [addr_lo, addr_hi, len]
for length in [1, 2, 4, 8, 16, 32, 64, 128]:
    r = hci_cmd(0xFC09, bytes([0x00, 0x00, length]))
    print(f"  FC09(00,00,{length:02X}): resp_len={len(r) if r else 0}, {fmt(r[:20]) if r else 'NONE'}")

# Read at addr 0x0094 with various lengths  
print("\n  FC09 at addr=0x0094:")
r = hci_cmd(0xFC09, bytes([0x94, 0x00, 0x04]))
print(f"  {fmt(r)}")

# === Scan ALL vendor opcodes ===
print("\n=== Vendor opcode scan FC00-FCFF ===")
print("  (showing opcodes that work or return >7 bytes)\n")

working = []
for ocf in range(256):
    opcode = 0xFC00 | ocf
    # Skip ones we already know
    if ocf in [0x03]:  # Skip HCI_Reset
        continue
    
    # Try with 3-byte params (read-like)
    r = hci_cmd(opcode, bytes([0x00, 0x00, 0x10]), wait=0.25)
    
    status = r[6] if r and len(r) > 6 else -1
    resp_len = len(r) if r else 0
    has_bt_addr = b'\x40\xBC\x32' in r if r else False
    
    interesting = False
    label = ""
    
    if has_bt_addr:
        label = " *** BT ADDR FOUND!"
        interesting = True
    elif status == 0 and resp_len > 10:
        label = f" data={fmt(r[7:min(20,resp_len)])}"
        interesting = True
    elif status == 0:
        label = " OK"
        interesting = True
    
    if interesting:
        working.append(ocf)
        print(f"  FC{ocf:02X}: status={status}, len={resp_len}{label}")

# === For working opcodes, try with EEPROM address params ===
print(f"\n=== Retry {len(working)} working opcodes with EEPROM addr params ===")
for ocf in working:
    opcode = 0xFC00 | ocf
    
    # Try addr=0x0000, len=16 
    r = hci_cmd(opcode, bytes([0x00, 0x00, 0x10]), wait=0.3)
    # Try addr=0x0094, len=4
    r2 = hci_cmd(opcode, bytes([0x94, 0x00, 0x04]), wait=0.3)
    
    data1 = fmt(r[7:min(25,len(r))]) if r and len(r) > 7 else ""
    data2 = fmt(r2[7:min(25,len(r2))]) if r2 and len(r2) > 7 else ""
    
    if data1 != data2:  # different data for different addresses = memory read!
        print(f"  FC{ocf:02X}:")
        print(f"    addr=0000: {data1}")
        print(f"    addr=0094: {data2}")
        if r2 and b'\x73' in r2[7:12]:
            print(f"    *** 0x73 found at addr 0x0094!")

print("\nDone.")
s.close()
