"""
Analyze the 2026-09-25 second DMS capture (with disconnect/reconnect).
Focus on:
1. Decode ALL IOCTL commands (reads + writes + connect/disconnect)
2. Extract the 08 03 / 0B 03 serial data records — these carry actual HCI bytes
3. Reconstruct the full command sequence chronologically
"""
import os, struct

LOG_PATH = os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-085143.dmslog8")

data = open(LOG_PATH, 'rb').read()
print(f"File: {os.path.basename(LOG_PATH)}")
print(f"File size: {len(data)} bytes\n")

# Find start of active region (first IOCTL marker)
first_ioctl = None
for i in range(len(data) - 3):
    if data[i] == 0x02 and data[i+1] == 0xFF and data[i+2] == 0x0F:
        first_ioctl = i
        break

# Also find first 08 03 or 0B 03 record
first_serial = None
for i in range(len(data) - 4):
    if ((data[i] == 0x08 or data[i] == 0x0B) and data[i+1] == 0x03 
        and data[i+2] == 0x00 and data[i+3] == 0x00):
        first_serial = i
        break

START = min(first_ioctl or len(data), first_serial or len(data)) - 64
START = max(0, START)
print(f"Active region starts around 0x{START:05X}")

# === 1. Extract ALL events chronologically ===
# We'll look for three types of records and sort by file position:
#   A. IOCTL address commands: [0A|0B] 00 05 02 [addr32] [len32] [data?]
#   B. 0803 records (PC -> BM64 serial writes)
#   C. 0B03 records (BM64 -> PC serial reads / responses)

events = []

# Type A: IOCTL address commands
for i in range(START, len(data) - 12):
    if data[i+1] == 0x00 and data[i+2] == 0x05 and data[i+3] == 0x02:
        cmd = data[i]
        addr = struct.unpack_from('<I', data, i+4)[0]
        length = struct.unpack_from('<I', data, i+8)[0]
        if cmd == 0x0A and length <= 256:
            events.append((i, 'IOCTL_READ', f"addr=0x{addr:04X} len={length}"))
        elif cmd == 0x0B and length <= 256:
            wdata = data[i+12:i+12+length]
            events.append((i, 'IOCTL_WRITE', f"addr=0x{addr:04X} len={length} data=[{' '.join(f'{b:02X}' for b in wdata)}]"))

# Type B/C: Serial data records
for i in range(START, len(data) - 5):
    if data[i+1] == 0x03 and data[i+2] == 0x00 and data[i+3] == 0x00:
        if data[i] == 0x08:
            # 08 03 = Write to device (PC -> BM64)
            # Extract the serial payload bytes
            payload = data[i+4:i+20]
            hexstr = ' '.join(f'{b:02X}' for b in payload)
            events.append((i, 'SERIAL_TX', hexstr))
        elif data[i] == 0x0B:
            payload = data[i+4:i+20]
            hexstr = ' '.join(f'{b:02X}' for b in payload)
            events.append((i, 'SERIAL_RX', hexstr))

# Sort by file position (chronological)
events.sort(key=lambda e: e[0])

print(f"\nTotal events found: {len(events)}\n")
print("=" * 100)
print("CHRONOLOGICAL EVENT LOG")
print("=" * 100)

for off, etype, detail in events:
    print(f"  0x{off:05X}  {etype:14s}  {detail}")

# === 2. Decode the 0803/0B03 serial records more carefully ===
# The data after 0803/0B03 follows this pattern:
#   [08|0B] 03 00 00 00 [serial_bytes...] [then FILETIME records]
# We need to figure out where the serial payload ends and the FILETIME starts
print("\n\n" + "=" * 100)
print("SERIAL RECORD DEEP DECODE")
print("=" * 100)

for i in range(START, len(data) - 20):
    if data[i+1] == 0x03 and data[i+2] == 0x00 and data[i+3] == 0x00:
        if data[i] not in (0x08, 0x0B):
            continue
        direction = "TX(PC->BM64)" if data[i] == 0x08 else "RX(BM64->PC)"
        
        # The 5th byte (data[i+4]) is 0x00, so skip first 5 bytes of the record header
        # Then look for the next FILETIME marker to find end of serial data
        # Serial data starts at i+4 after the 00
        # Actually, let's look at the byte right after "xx 03 00 00 00"
        serial_start = i + 5
        
        # Find next FILETIME (xx xx xx xx 04 4D DD 01)
        ft_pos = None
        for j in range(serial_start, min(serial_start + 60, len(data) - 8)):
            if data[j+4:j+8] == b'\x04\x4D\xDD\x01':
                ft_pos = j
                break
        
        if ft_pos:
            serial_end = ft_pos
            serial_bytes = data[serial_start:serial_end]
        else:
            serial_bytes = data[serial_start:serial_start+16]
        
        hexstr = ' '.join(f'{b:02X}' for b in serial_bytes)
        print(f"\n  0x{i:05X}  {direction}  serial_bytes=[{hexstr}]  ({len(serial_bytes)} bytes)")
        
        # Try to decode as HCI
        if len(serial_bytes) >= 4 and serial_bytes[0] == 0x01:
            opcode = serial_bytes[1] | (serial_bytes[2] << 8)
            plen = serial_bytes[3]
            params = serial_bytes[4:4+plen] if len(serial_bytes) > 4 else b''
            phex = ' '.join(f'{b:02X}' for b in params)
            print(f"           HCI CMD opcode=0x{opcode:04X} paramLen={plen} params=[{phex}]")
        elif len(serial_bytes) >= 3 and serial_bytes[0] == 0x04:
            evt_code = serial_bytes[1]
            evt_len = serial_bytes[2]
            payload = serial_bytes[3:3+evt_len] if len(serial_bytes) > 3 else b''
            phex = ' '.join(f'{b:02X}' for b in payload)
            extra = ""
            if evt_code == 0x0E and len(payload) >= 3:
                op = payload[1] | (payload[2] << 8)
                extra = f" (CmdComplete for 0x{op:04X})"
            print(f"           HCI EVT code=0x{evt_code:02X} len={evt_len}{extra} data=[{phex}]")


# === 3. Summary ===
print("\n\n" + "=" * 100)
print("PROTOCOL SUMMARY")
print("=" * 100)

reads = [(o, d) for o, t, d in events if t == 'IOCTL_READ']
writes = [(o, d) for o, t, d in events if t == 'IOCTL_WRITE']
print(f"\nIOCTL READs:  {len(reads)}")
for off, detail in reads:
    print(f"  0x{off:05X}: {detail}")
print(f"\nIOCTL WRITEs: {len(writes)}")
for off, detail in writes:
    print(f"  0x{off:05X}: {detail}")

serial_tx = [(o, d) for o, t, d in events if t == 'SERIAL_TX']
serial_rx = [(o, d) for o, t, d in events if t == 'SERIAL_RX']
print(f"\nSerial TX records: {len(serial_tx)}")
print(f"Serial RX records: {len(serial_rx)}")
