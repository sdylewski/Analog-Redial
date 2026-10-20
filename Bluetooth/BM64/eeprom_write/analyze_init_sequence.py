"""
Extract the INITIALIZATION sequence from the DMS capture — what does isUpdate
send BEFORE the first EEPROM read? This tells us what handshake/setup the
BM64 needs after entering IBDK mode.

Focus: first 50 events, and carefully separate DMS record metadata from
actual serial bytes using record-length heuristics.
"""
import os, struct

LOG_PATH = os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-085143.dmslog8")
data = open(LOG_PATH, 'rb').read()
print(f"File size: {len(data)} bytes\n")

# Find start of active data
START = 0
for i in range(len(data) - 4):
    if data[i] == 0x02 and data[i+1] == 0xFF and data[i+2] == 0x0F:
        START = max(0, i - 64)
        break
    if (data[i] in (0x08, 0x0B)) and data[i+1] == 0x03 and data[i+2] == 0x00:
        START = max(0, i - 64)
        break

print(f"Active data starts around 0x{START:05X}")

# === Approach: dump ALL record-like structures from the start ===
# DMS records seem to have:
#   [02 FF 0F ...] = IOCTL/DeviceIoControl records
#   [08 03 00 00 ...] = some type of record  
#   [0B 03 00 00 ...] = another type of record
#
# Instead of guessing the format, let's just dump raw hex in 
# 256-byte chunks with annotations for pattern recognition.

print("\n" + "=" * 100)
print("FIRST 4096 BYTES OF ACTIVE REGION (raw hex)")
print("=" * 100)

end = min(len(data), START + 4096)
for row in range(START, end, 16):
    n = min(16, end - row)
    hexpart = ' '.join(f'{data[row+j]:02X}' for j in range(n))
    ascpart = ''.join(chr(data[row+j]) if 32 <= data[row+j] < 127 else '.'
                      for j in range(n))
    markers = []
    for j in range(n):
        pos = row + j
        if pos + 3 <= len(data):
            if data[pos] == 0x02 and data[pos+1] == 0xFF and data[pos+2] == 0x0F:
                markers.append(f"IOCTL@{j}")
            if data[pos] == 0x08 and data[pos+1] == 0x03 and data[pos+2] == 0x00 and data[pos+3] == 0x00:
                markers.append(f"0803@{j}")
            if data[pos] == 0x0B and data[pos+1] == 0x03 and data[pos+2] == 0x00 and data[pos+3] == 0x00:
                markers.append(f"0B03@{j}")
        # FILETIME marker: xx xx xx xx [04|05] 4D DD 01  
        if pos + 4 <= len(data) and data[pos+1] == 0x4D and data[pos+2] == 0xDD and data[pos+3] == 0x01:
            markers.append(f"FT@{j}")
    marker_str = "  << " + ", ".join(markers) if markers else ""
    print(f"  {row:05X}: {hexpart:<48s}  {ascpart}{marker_str}")

# === Also: try to understand the IOCTL structure better ===
print("\n\n" + "=" * 100)
print("ALL IOCTL (02 FF 0F) RECORDS — first 20")
print("=" * 100)

count = 0
for i in range(len(data) - 20):
    if data[i] == 0x02 and data[i+1] == 0xFF and data[i+2] == 0x0F:
        # Dump 32 bytes starting at the IOCTL marker
        ctx = data[i:min(i+48, len(data))]
        hexstr = ' '.join(f'{b:02X}' for b in ctx)
        print(f"\n  IOCTL @ 0x{i:05X}: {hexstr}")
        
        # The bytes after 02 FF 0F seem to be: [XX 00] [YY ZZ] [command bytes...]
        # Let's decode what we can
        if len(ctx) >= 6:
            print(f"    bytes[3:6] = {ctx[3]:02X} {ctx[4]:02X} {ctx[5]:02X}")
        
        count += 1
        if count >= 20:
            break
