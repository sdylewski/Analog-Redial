"""
BM64 HCI probe round 2 — figure out the memory type selection command
so we can read/write EEPROM from ESP32.

From round 1 we know:
  - FC09 = read memory (but reads flash by default, got 0x55)
  - FC0B = some control command (returns short ack)
  - isUpdate's EEPROM read at 0x0000 returned: 40BC32D0B744...
  - isUpdate has memory type dropdown: flash/eeprom/emcu

Theory: FC0B selects the memory type, then FC09 reads from it.
We need to find the right FC0B params to select "eeprom".

Run: python -u probe_hci2.py
Arduino IDE must be FULLY QUIT. BOOT_MODE 0 must be uploaded.
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

def open_port():
    try:
        s = serial.Serial(PORT, BAUD, timeout=2)
        time.sleep(2)  # ESP32 boots + enters IBDK
        s.reset_input_buffer()
        return s
    except serial.SerialException as e:
        print(f"FAILED: {e}\nQuit Arduino IDE!")
        sys.exit(1)

def hci_cmd(s, opcode, params=b'', wait=0.5):
    ocf_lo = opcode & 0xFF
    ocf_hi = (opcode >> 8) & 0xFF
    cmd = bytes([0x01, ocf_lo, ocf_hi, len(params)]) + params
    s.reset_input_buffer()
    s.write(cmd)
    time.sleep(wait)
    return s.read(s.in_waiting or 200)

def fmt(data):
    return ' '.join(f'{b:02X}' for b in data) if data else 'EMPTY'

print("Opening port...")
s = open_port()

# HCI Reset first
print("HCI Reset...")
r = hci_cmd(s, 0x0C03, wait=1.0)
print(f"  -> {fmt(r)}")
time.sleep(0.5)
s.read(s.in_waiting)  # drain

# Baseline: FC09 read 16 bytes from addr 0x0000 (should get flash = 0x55)
print("\n=== Baseline: FC09 read addr 0x0000, len 16 (no type select) ===")
r = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]))
print(f"  -> {fmt(r)}")

# Now try FC0B with different single-byte params to select memory type
# then read with FC09 to see if the data changes
print("\n=== Try FC0B type select, then FC09 read ===")
print("  Looking for 40 BC 32 D0 B7 44 (BT address at EEPROM 0x0000)\n")

for type_byte in range(0x00, 0x10):
    # Send FC0B with this type byte
    r1 = hci_cmd(s, 0xFC0B, bytes([type_byte]), wait=0.3)
    # Then read
    r2 = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.5)
    
    # Check if we got our BT address back
    has_bt_addr = b'\x40\xBC\x32\xD0\xB7\x44' in r2
    marker = " *** BT ADDR FOUND! ***" if has_bt_addr else ""
    
    # Only print if response differs from baseline (0x55) or has BT addr
    if has_bt_addr or (r2 and b'\x55\x55\x55\x55' not in r2):
        print(f"  FC0B({type_byte:02X}) -> {fmt(r1)}")
        print(f"  FC09 read   -> {fmt(r2)}{marker}")
        print()

# Try FC0B with 2-byte params [type, subtype]
print("=== Try FC0B with 2-byte params [type, subtype] ===\n")
for t in range(0x04):
    for sub in range(0x04):
        r1 = hci_cmd(s, 0xFC0B, bytes([t, sub]), wait=0.3)
        r2 = hci_cmd(s, 0xFC09, bytes([0x00, 0x00, 0x10]), wait=0.5)
        has_bt_addr = b'\x40\xBC\x32\xD0\xB7\x44' in r2
        if has_bt_addr or (r2 and b'\x55\x55\x55\x55' not in r2):
            print(f"  FC0B({t:02X},{sub:02X}) -> {fmt(r1)}")
            print(f"  FC09 read      -> {fmt(r2)}")
            if has_bt_addr:
                print(f"  *** EEPROM ACCESS CONFIRMED! ***")
            print()

# Also try if there's a completely different read opcode for EEPROM
print("=== Try alternative read opcodes after FC0B(01) ===\n")
hci_cmd(s, 0xFC0B, bytes([0x01]), wait=0.3)  # try type=1

for ocf in [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A,
            0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15]:
    opcode = 0xFC00 | ocf
    r = hci_cmd(s, opcode, bytes([0x00, 0x00, 0x10]), wait=0.5)
    if r and b'\x40\xBC' in r:
        print(f"  *** FC{ocf:02X} returned BT addr! -> {fmt(r)}")
    elif r and len(r) > 10 and r != b'\x04\x10\x01\x10':
        # Long response that's not just a hardware error
        events = []
        i = 0
        while i < len(r):
            if r[i] == 0x04 and i+2 < len(r):
                elen = r[i+2]
                if r[i+1] == 0x0E and elen > 4:  # Command Complete with data
                    print(f"  FC{ocf:02X} -> {fmt(r)}")
                    break
            i += 1

print("\nDone.")
s.close()
