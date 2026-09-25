"""
Theory: EEPROM commands travel as HCI ACL Data (type 02) to handle 0xFFF,
not as HCI commands (type 01). The '02 FF 0F' in the IOCTL structures IS
the ACL header.

This script:
1. Finds ALL records prefixed by '00 04 00 00 00' (outgoing serial data marker)
2. Shows the first 20 bytes of each to identify HCI packet types
3. Separately finds records prefixed by '08 04 00 00 00' (echo/duplicate marker)
4. Maps the complete init sequence and transition to EEPROM operations
"""
import os, struct

LOG_PATH = os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-085143.dmslog8")
data = open(LOG_PATH, 'rb').read()

# Find all '00 04 00 00 00' markers (outgoing serial data?)
print("=" * 100)
print("ALL '00 04 00 00 00' RECORDS (potential outgoing serial data)")
print("=" * 100)

count = 0
for i in range(0x13500, len(data) - 30):
    if (data[i] == 0x00 and data[i+1] == 0x04 and data[i+2] == 0x00 
        and data[i+3] == 0x00 and data[i+4] == 0x00):
        # Check it's not inside a timestamp (byte before shouldn't be part of FILETIME)
        # Heuristic: byte at i-1 should be 0x00 (end of previous record's padding)
        if i > 0 and data[i-1] != 0x00:
            continue
        
        following = data[i+5:i+30]
        hexstr = ' '.join(f'{b:02X}' for b in following)
        
        # Identify HCI packet type
        pkt_type = following[0] if len(following) > 0 else -1
        pkt_desc = {0x01: "HCI_CMD", 0x02: "HCI_ACL", 0x03: "HCI_SCO", 0x04: "HCI_EVT"}.get(pkt_type, f"type={pkt_type:#04x}")
        
        print(f"  0x{i:05X}: [{pkt_desc}] {hexstr}")
        count += 1

print(f"\nTotal: {count} records\n")

# Also find '08 04' records
print("=" * 100)
print("ALL '08 04 00 00 00' RECORDS (echo/duplicate)")
print("=" * 100)

count = 0
for i in range(0x13500, len(data) - 30):
    if (data[i] == 0x08 and data[i+1] == 0x04 and data[i+2] == 0x00 
        and data[i+3] == 0x00 and data[i+4] == 0x00):
        following = data[i+5:i+30]
        hexstr = ' '.join(f'{b:02X}' for b in following)
        pkt_type = following[0] if len(following) > 0 else -1
        pkt_desc = {0x01: "HCI_CMD", 0x02: "HCI_ACL", 0x03: "HCI_SCO", 0x04: "HCI_EVT"}.get(pkt_type, f"type={pkt_type:#04x}")
        print(f"  0x{i:05X}: [{pkt_desc}] {hexstr}")
        count += 1

print(f"\nTotal: {count} records\n")

# Look for HCI ACL packets specifically (02 FF 0F anywhere)
print("=" * 100)
print("ALL '02 FF 0F' PATTERNS (potential ACL to handle 0xFFF)")
print("=" * 100)

for i in range(0x13500, len(data) - 20):
    if data[i] == 0x02 and data[i+1] == 0xFF and data[i+2] == 0x0F:
        ctx = data[i:i+24]
        hexstr = ' '.join(f'{b:02X}' for b in ctx)
        # Parse ACL header
        data_len = (ctx[4] << 8) | ctx[3] if len(ctx) > 4 else 0
        payload = ctx[5:5+min(data_len, 19)]
        print(f"  0x{i:05X}: {hexstr}")
        print(f"           data_len={data_len}, payload={payload.hex()}")
