"""
Focused analysis: dump raw hex around each IOCTL_READ event to separate
DMS record metadata from actual serial wire bytes.

Goal: figure out what bytes isUpdate ACTUALLY sends to the BM64, because the
'0A 00 05 02' pattern may be IOCTL control data, not serial data.
"""
import os, struct

LOG_PATH = os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-085143.dmslog8")
data = open(LOG_PATH, 'rb').read()
print(f"File size: {len(data)} bytes\n")

# Find all IOCTL_READ positions
reads = []
writes = []
for i in range(len(data) - 12):
    if data[i+1] == 0x00 and data[i+2] == 0x05 and data[i+3] == 0x02:
        cmd = data[i]
        addr = struct.unpack_from('<I', data, i+4)[0]
        length = struct.unpack_from('<I', data, i+8)[0]
        if cmd == 0x0A and length <= 256:
            reads.append((i, addr, length))
        elif cmd == 0x0B and length <= 256:
            wdata = data[i+12:i+12+length]
            writes.append((i, addr, length, wdata))

# For each IOCTL_READ, dump a wide hex window around it
print("=" * 100)
print("RAW HEX CONTEXT AROUND EACH IOCTL_READ")
print("=" * 100)

for idx, (off, addr, length) in enumerate(reads):
    print(f"\n--- IOCTL_READ #{idx}: addr=0x{addr:04X} len={length} @ offset 0x{off:05X} ---")
    # Dump 256 bytes before and 512 bytes after
    start = max(0, off - 256)
    end = min(len(data), off + 512)
    for row in range(start, end, 16):
        n = min(16, end - row)
        hexpart = ' '.join(f'{data[row+j]:02X}' for j in range(n))
        ascpart = ''.join(chr(data[row+j]) if 32 <= data[row+j] < 127 else '.'
                          for j in range(n))
        marker = ""
        if row <= off < row + 16:
            marker = "  <<<< IOCTL_READ HERE"
        # Mark known DMS record types
        for j in range(n - 1):
            if data[row+j] == 0x08 and data[row+j+1] == 0x03:
                marker += f"  << 0803@+{j}"
            if data[row+j] == 0x0B and data[row+j+1] == 0x03:
                marker += f"  << 0B03@+{j}"
            if data[row+j] == 0x02 and row+j+2 < end and data[row+j+1] == 0xFF and data[row+j+2] == 0x0F:
                marker += f"  << IOCTL@+{j}"
        print(f"  {row:05X}: {hexpart:<48s}  {ascpart}{marker}")
    # Only show first 2 reads to keep output manageable
    if idx >= 1:
        break

print("\n\n" + "=" * 100)
print("RAW HEX CONTEXT AROUND FIRST IOCTL_WRITE")
print("=" * 100)

if writes:
    off, addr, length, wdata = writes[0]
    print(f"\n--- IOCTL_WRITE #0: addr=0x{addr:04X} len={length} data={wdata.hex()} @ offset 0x{off:05X} ---")
    start = max(0, off - 256)
    end = min(len(data), off + 512)
    for row in range(start, end, 16):
        n = min(16, end - row)
        hexpart = ' '.join(f'{data[row+j]:02X}' for j in range(n))
        ascpart = ''.join(chr(data[row+j]) if 32 <= data[row+j] < 127 else '.'
                          for j in range(n))
        marker = ""
        if row <= off < row + 16:
            marker = "  <<<< IOCTL_WRITE HERE"
        for j in range(n - 1):
            if data[row+j] == 0x08 and data[row+j+1] == 0x03:
                marker += f"  << 0803@+{j}"
            if data[row+j] == 0x0B and data[row+j+1] == 0x03:
                marker += f"  << 0B03@+{j}"
            if data[row+j] == 0x02 and row+j+2 < end and data[row+j+1] == 0xFF and data[row+j+2] == 0x0F:
                marker += f"  << IOCTL@+{j}"
        print(f"  {row:05X}: {hexpart:<48s}  {ascpart}{marker}")

# Also: scan for the actual EEPROM values to see their context
print("\n\n" + "=" * 100)
print("LOCATIONS OF KNOWN EEPROM VALUES IN RX CONTEXT")
print("=" * 100)

# We know from the script output that response bytes containing the
# EEPROM value appear in patterns like: 10 01 07 00 00 00 01 00 00 00 [DATA]
# Search for this pattern with DATA=0x09 and DATA=0x0A
for pattern_name, pattern in [
    ("READ resp 0x0A", bytes([0x10, 0x01, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A])),
    ("READ resp 0x09", bytes([0x10, 0x01, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x09])),
    ("WRITE resp?",    bytes([0x11, 0x01, 0x02, 0x00, 0x00, 0x00])),
]:
    positions = []
    for i in range(len(data) - len(pattern)):
        if data[i:i+len(pattern)] == pattern:
            positions.append(i)
    print(f"\n  '{pattern_name}' found at: {[f'0x{p:05X}' for p in positions]}")
    for p in positions[:3]:
        ctx = data[max(0,p-32):p+48]
        offset_start = max(0, p-32)
        print(f"    context from 0x{offset_start:05X}:")
        for row_off in range(0, len(ctx), 16):
            chunk = ctx[row_off:row_off+16]
            hexpart = ' '.join(f'{b:02X}' for b in chunk)
            print(f"      {offset_start+row_off:05X}: {hexpart}")
