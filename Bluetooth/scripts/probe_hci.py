"""
Probe BM64 HCI vendor-specific commands in IBDK mode.
Discovers the read/write EEPROM commands so we can replicate them on ESP32.

Run with: python -u probe_hci.py
Make sure Arduino IDE is FULLY QUIT and the auto-bridge sketch (BOOT_MODE 0)
is uploaded.
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

def open_port():
    try:
        s = serial.Serial(PORT, BAUD, timeout=2)
        time.sleep(2)  # Wait for ESP32 to boot and enter IBDK mode
        s.reset_input_buffer()  # Drain BM64 boot noise
        return s
    except serial.SerialException as e:
        print(f"FAILED to open {PORT}: {e}")
        print("Make sure Arduino IDE is fully QUIT!")
        sys.exit(1)

def send_hci_cmd(s, opcode, params=b''):
    """Send an HCI command and return the response."""
    ocf = opcode & 0x03FF
    ogf = (opcode >> 10) & 0x3F
    ocf_lo = opcode & 0xFF
    ocf_hi = (opcode >> 8) & 0xFF
    cmd = bytes([0x01, ocf_lo, ocf_hi, len(params)]) + params
    
    s.reset_input_buffer()
    s.write(cmd)
    time.sleep(0.5)
    
    resp = s.read(s.in_waiting or 100)
    return resp

def format_hex(data):
    return ' '.join(f'{b:02X}' for b in data) if data else 'EMPTY'

def parse_hci_event(data):
    """Parse HCI event packets from response data."""
    events = []
    i = 0
    while i < len(data):
        if data[i] == 0x04:  # HCI Event
            if i + 2 < len(data):
                evt_code = data[i+1]
                param_len = data[i+2]
                if i + 3 + param_len <= len(data):
                    params = data[i+3:i+3+param_len]
                    events.append((evt_code, params))
                    i += 3 + param_len
                    continue
        i += 1
    return events

print(f"{'='*70}")
print(f"BM64 HCI Vendor-Specific Command Probe")
print(f"{'='*70}")
print(f"\nOpening {PORT}...")

s = open_port()

# Step 1: HCI Reset
print("\n--- Step 1: HCI Reset ---")
resp = send_hci_cmd(s, 0x0C03)  # HCI_Reset
print(f"  HCI_Reset -> {format_hex(resp)}")
events = parse_hci_event(resp)
for evt_code, params in events:
    print(f"  Event 0x{evt_code:02X}: {format_hex(params)}")
time.sleep(0.5)

# Drain any extra events
extra = s.read(s.in_waiting or 100)
if extra:
    print(f"  Extra: {format_hex(extra)}")

# Step 2: Try to read chip info with vendor-specific commands
# ISSC vendor-specific commands use OGF=0x3F (opcode 0xFCxx)
print("\n--- Step 2: Probe vendor-specific READ commands ---")
print("  Trying to read 16 bytes from EEPROM address 0x0000...\n")

# Try various OCFs that might be memory read commands
# Format guesses for read: [memory_type, addr (2-4 bytes), length (1-2 bytes)]
test_opcodes = []

# Common ISSC vendor-specific opcodes
for ocf in range(0x00, 0x20):
    test_opcodes.append(0xFC00 | ocf)

# Also try some higher ones
for ocf in [0x20, 0x21, 0x22, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0]:
    test_opcodes.append(0xFC00 | ocf)

for opcode in test_opcodes:
    # Try with param format: [addr_hi, addr_lo, length] (3 bytes)
    params = bytes([0x00, 0x00, 0x10])
    resp = send_hci_cmd(s, opcode, params)
    
    if resp and resp != b'\x04\x10\x01\x10':  # Skip hardware error (noise)
        events = parse_hci_event(resp)
        interesting = False
        for evt_code, params_data in events:
            # Command Complete with actual data (not just error)
            if evt_code == 0x0E and len(params_data) > 4:
                interesting = True
            # Any non-hardware-error event
            if evt_code != 0x10:
                interesting = True
        
        if interesting:
            print(f"  *** Opcode 0x{opcode:04X}: {format_hex(resp)}")
            for evt_code, params_data in events:
                print(f"      Event 0x{evt_code:02X}: {format_hex(params_data)}")

# Step 3: Try different parameter formats for a few promising opcodes
print("\n--- Step 3: Try different param formats ---")

# Format A: [type, addr_hi, addr_lo, len_hi, len_lo]
# Format B: [addr_3, addr_2, addr_1, addr_0, len_1, len_0]
# Format C: [addr_lo, addr_hi, len_lo, len_hi]
# Format D: just [addr_lo, addr_hi, len]

param_formats = {
    "type+addr16+len16":    bytes([0x01, 0x00, 0x00, 0x00, 0x10]),
    "type+addr16+len8":     bytes([0x01, 0x00, 0x00, 0x10]),
    "addr32LE+len16LE":     bytes([0x00, 0x00, 0x00, 0x00, 0x10, 0x00]),
    "addr16LE+len16LE":     bytes([0x00, 0x00, 0x10, 0x00]),
    "addr16BE+len8":        bytes([0x00, 0x00, 0x10]),
    "addr16LE+len8":        bytes([0x00, 0x00, 0x10]),
    "just_addr16":          bytes([0x00, 0x00]),
    "empty":                bytes([]),
    "type0+addr+len":       bytes([0x00, 0x00, 0x00, 0x10]),
    "type2+addr+len":       bytes([0x02, 0x00, 0x00, 0x10]),
}

# Try a few likely opcodes with all param formats
for ocf in [0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x12]:
    opcode = 0xFC00 | ocf
    for fmt_name, params in param_formats.items():
        resp = send_hci_cmd(s, opcode, params)
        if resp and resp != b'\x04\x10\x01\x10':
            events = parse_hci_event(resp)
            for evt_code, params_data in events:
                if evt_code == 0x0E and len(params_data) > 5:
                    print(f"  *** Opcode 0x{opcode:04X} fmt={fmt_name}")
                    print(f"      Sent params: {format_hex(params)}")
                    print(f"      Response: {format_hex(resp)}")
                    print(f"      Event 0x{evt_code:02X} data: {format_hex(params_data)}")

# Step 4: Read EEPROM at 0x0094 to verify headphone DAC byte
# If we found the read command above, great. If not, try the known
# isupdate.exe "Read" approach by sending what it might send
print("\n--- Step 4: Known-good EEPROM read test ---")
print("  (Looking for 0x73 at address 0x0094 to confirm headphone DAC config)\n")

# Try the most common ISSC read formats
read_attempts = [
    ("FC01 addr32LE len16LE",   0xFC01, bytes([0x94, 0x00, 0x00, 0x00, 0x10, 0x00])),
    ("FC01 addr16BE len8",      0xFC01, bytes([0x00, 0x94, 0x10])),
    ("FC01 addr16LE len8",      0xFC01, bytes([0x94, 0x00, 0x10])),
    ("FC01 type1 addr16 len",   0xFC01, bytes([0x01, 0x00, 0x94, 0x10])),
    ("FC02 addr32LE len16LE",   0xFC02, bytes([0x94, 0x00, 0x00, 0x00, 0x10, 0x00])),
    ("FC0D addr32LE len16LE",   0xFC0D, bytes([0x94, 0x00, 0x00, 0x00, 0x10, 0x00])),
    ("FC09 addr32LE len16LE",   0xFC09, bytes([0x94, 0x00, 0x00, 0x00, 0x10, 0x00])),
]

for name, opcode, params in read_attempts:
    resp = send_hci_cmd(s, opcode, params)
    if resp and resp != b'\x04\x10\x01\x10':
        events = parse_hci_event(resp)
        for evt_code, params_data in events:
            if evt_code == 0x0E and len(params_data) > 5:
                print(f"  >>> HIT: {name}")
                print(f"      Response: {format_hex(resp)}")
                # Check if 0x73 appears in the response
                if 0x73 in params_data:
                    print(f"      *** Found 0x73 in response! EEPROM read likely works! ***")

print(f"\n{'='*70}")
print("Done probing. Closing port.")
s.close()
