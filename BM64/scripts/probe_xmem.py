"""
XMem = External Memory = where EEPROM data lives at runtime!
EEPROM_Tool.exe uses XMem addresses like 0xB8E3.
EEPROM base might be 0xB800 (or nearby).

EEPROM addr 0x0094 (headphone DAC=0x73) might be at XMem 0xB894.
EEPROM addr 0x0000 (BT addr: 40 BC 32 D0 B7 44) might be at XMem 0xB800.

Try FC09 and FC0B at XMem addresses.
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
    time.sleep(0.05)
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

BT_ADDR = bytes([0x40, 0xBC, 0x32, 0xD0, 0xB7, 0x44])

# === Try FC09 at XMem base addresses ===
print("\n=== FC09 at XMem address ranges ===")
print("  Looking for BT addr: 40 BC 32 D0 B7 44")
print("  Looking for 0x73 at EEPROM offset +0x94\n")

# Candidate EEPROM bases in XMem
bases = [0xB800, 0xB000, 0xA000, 0x8000, 0x9000, 0xC000, 0xD000, 
         0xE000, 0xF000, 0x1000, 0x2000, 0x3000, 0x4000, 0x5000,
         0x6000, 0x7000, 0xB400, 0xB600, 0xB900, 0xBA00, 0xBB00, 0xBC00]

for base in bases:
    # Read 8 bytes from base address
    r = hci_cmd(0xFC09, bytes([base & 0xFF, (base >> 8) & 0xFF, 0x08]))
    data = b''
    if r and len(r) > 10:
        data = r[10:]
    
    # Also read at base + 0x94
    addr94 = base + 0x94
    r94 = hci_cmd(0xFC09, bytes([addr94 & 0xFF, (addr94 >> 8) & 0xFF, 0x01]))
    val94 = r94[10] if r94 and len(r94) > 10 else None
    
    bt_found = BT_ADDR[:3] in data
    eep_found = val94 == 0x73 if val94 is not None else False
    
    if bt_found or eep_found or (data and data != b'\x55'*len(data) and data != b'\x00'*len(data) and data != b'\xFF'*len(data)):
        marker = ""
        if bt_found: marker += " *** BT ADDR! ***"
        if eep_found: marker += " *** EEPROM 0x73! ***"
        v94_str = f"0x{val94:02X}" if val94 is not None else "None"
        print(f"  base=0x{base:04X}: {fmt(data)}  [+94]={v94_str}{marker}")

# === Try wider scan with FC09 at 0x100 increments ===
print("\n=== FC09 wide scan (every 0x100) ===")
for base in range(0x0000, 0x10000, 0x100):
    r = hci_cmd(0xFC09, bytes([base & 0xFF, (base >> 8) & 0xFF, 0x08]), wait=0.15)
    data = r[10:18] if r and len(r) >= 18 else b''
    
    if BT_ADDR[:3] in data:
        print(f"  *** BT ADDR at 0x{base:04X}: {fmt(data)} ***")
        # Read more detail
        for offset in [0x00, 0x08, 0x10, 0x90, 0x94]:
            addr = base + offset
            r2 = hci_cmd(0xFC09, bytes([addr & 0xFF, (addr >> 8) & 0xFF, 0x08]))
            d2 = r2[10:] if r2 and len(r2) > 10 else b''
            print(f"    +{offset:02X} (0x{addr:04X}): {fmt(d2)}")
        break
    
    # Check for 0x73 at +0x94 offset (just spot check at key bases)
    if base % 0x800 == 0:
        addr94 = (base + 0x94) & 0xFFFF
        r94 = hci_cmd(0xFC09, bytes([addr94 & 0xFF, (addr94 >> 8) & 0xFF, 0x01]), wait=0.15)
        val94 = r94[10] if r94 and len(r94) > 10 else None
        if val94 == 0x73:
            print(f"  *** EEPROM at base 0x{base:04X}! [+94]=0x73 ***")

print("\nDone.")
s.close()
