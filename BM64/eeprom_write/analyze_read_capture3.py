"""
Deep-dive into the 2026-09-25 DMS log structure.
The previous analyzer found 0 raw HCI commands — the serial bytes must be
wrapped in IOCTL records differently than the 2026-09-24 capture.

This script:
1. Dumps the first 512 bytes and key areas to understand the file format
2. Searches for known DMS record signatures
3. Looks for the IOCTL wrapping pattern from the hits we found
4. Searches for ALL occurrences of address 0x0348 (48 03) to trace R/W/R
"""
import os, struct

LOG_PATH = os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-084134.dmslog8")

data = open(LOG_PATH, 'rb').read()
print(f"File size: {len(data)} bytes\n")

# === 1. Dump header and structure ===
print("=" * 70)
print("FILE HEADER (first 256 bytes)")
print("=" * 70)
for row in range(0, 256, 16):
    hexpart = ' '.join(f'{data[row+i]:02X}' for i in range(min(16, len(data)-row)))
    ascpart = ''.join(chr(data[row+i]) if 32 <= data[row+i] < 127 else '.'
                      for i in range(min(16, len(data)-row)))
    print(f"  {row:04X}: {hexpart:<48s}  {ascpart}")

# === 2. Look at the IOCTL pattern we found ===
# The hits from analyzer2 all had: 02 FF 0F xx 00 xx 01 xx 00 05 02 48 03
# This looks like Windows DeviceIoControl record framing.
# Let's find ALL instances of "05 02" (which appeared before the address)
# and see what surrounds them
print("\n\n" + "=" * 70)
print("ALL OCCURRENCES OF '05 02' pattern (IOCTL framing?)")
print("=" * 70)
count = 0
for i in range(len(data) - 2):
    if data[i] == 0x05 and data[i+1] == 0x02:
        ctx_start = max(0, i - 32)
        ctx = data[ctx_start:i+32]
        hexstr = ' '.join(f'{b:02X}' for b in ctx)
        count += 1
        if count <= 30:
            print(f"  @ 0x{i:05X}: ...{hexstr}...")
        elif count == 31:
            print(f"  ... (truncating, showing first 30)")

# === 3. Look for the repeating IOCTL pattern ===
# From the hits: "00 04 00 00 00 02 FF 0F" appears multiple times
# Let's find all instances and see what varies
print(f"\n  Total '05 02' occurrences: {count}")

print("\n\n" + "=" * 70)
print("IOCTL SIGNATURE: '02 FF 0F' occurrences")
print("=" * 70)
ioctl_positions = []
for i in range(len(data) - 3):
    if data[i] == 0x02 and data[i+1] == 0xFF and data[i+2] == 0x0F:
        ioctl_positions.append(i)
print(f"Found {len(ioctl_positions)} IOCTL markers")
if ioctl_positions:
    print(f"First at 0x{ioctl_positions[0]:05X}, last at 0x{ioctl_positions[-1]:05X}")
    # Show spacing between them
    if len(ioctl_positions) > 1:
        spacings = [ioctl_positions[i+1] - ioctl_positions[i] for i in range(min(30, len(ioctl_positions)-1))]
        print(f"Spacings (first 30): {spacings}")

# === 4. Look for actual serial data between IOCTL records ===
# The IOCTL records have a fixed pattern. Real serial data would be between them.
# From the old capture analysis: records are [00 00 00 00][LEN][data]
# But maybe this capture uses a different record type

print("\n\n" + "=" * 70)
print("SEARCHING FOR WRITE/READ SERIAL DATA RECORDS")
print("=" * 70)

# Look for patterns that could be serial write/read records
# Try different record header signatures
patterns = {
    "WriteFile(0x08,0x03)": (b'\x08\x03', 8),
    "WriteFile(0x0B,0x03)": (b'\x0B\x03', 8),
    "RecordType 0x08": (b'\x08\x00\x00\x00', 16),
    "RecordType 0x0B": (b'\x0B\x00\x00\x00', 16),
    "RecordType 0x04": (b'\x04\x00\x00\x00', 16),
}

for name, (sig, ctx_len) in patterns.items():
    positions = []
    for i in range(len(data) - len(sig)):
        if data[i:i+len(sig)] == sig:
            positions.append(i)
    if positions:
        print(f"\n  '{name}' found {len(positions)} times")
        for pos in positions[:5]:
            ctx = data[pos:pos+ctx_len*2]
            hexstr = ' '.join(f'{b:02X}' for b in ctx)
            print(f"    @ 0x{pos:05X}: {hexstr}")

# === 5. Dump the area around each IOCTL block ===
# The data BETWEEN ioctl blocks is what we want
print("\n\n" + "=" * 70)
print("DATA BETWEEN IOCTL BLOCKS (first 20 gaps)")
print("=" * 70)

for gap_idx in range(min(20, len(ioctl_positions) - 1)):
    start = ioctl_positions[gap_idx]
    end = ioctl_positions[gap_idx + 1]
    gap_size = end - start
    # Each IOCTL record: look for what comes after "02 FF 0F xx xx xx xx xx xx xx xx xx"
    # Dump the full inter-IOCTL region
    region = data[start:end]
    hexstr = ' '.join(f'{b:02X}' for b in region[:80])
    if len(region) > 80:
        hexstr += f' ... ({len(region)} bytes total)'
    print(f"\n  Gap {gap_idx}: 0x{start:05X}->0x{end:05X} ({gap_size} bytes)")
    print(f"    {hexstr}")

# === 6. Look for FILETIME markers near serial data ===
# FILETIME has 04 4D DD 01 as high bytes (from the proximity search hits)
print("\n\n" + "=" * 70)
print("FILETIME MARKERS (xx xx xx xx 04 4D DD 01)")
print("=" * 70)
ft_positions = []
for i in range(len(data) - 8):
    if data[i+4:i+8] == b'\x04\x4D\xDD\x01':
        ft_positions.append(i)
        ft = struct.unpack_from('<Q', data, i)[0]
        # Convert FILETIME to readable (100ns intervals since 1601-01-01)
        # Just show the raw value and surrounding context
        if len(ft_positions) <= 20:
            ctx = data[max(0, i-8):i+16]
            hexstr = ' '.join(f'{b:02X}' for b in ctx)
            print(f"  @ 0x{i:05X}: {hexstr}")

print(f"\n  Total FILETIME markers: {len(ft_positions)}")

# === 7. Structured record parsing — try DMS8 format ===
# DMS8 format may use: [record_type:4][timestamp:8][data_direction:4][data_length:4][data]
print("\n\n" + "=" * 70)
print("TRYING STRUCTURED DMS8 RECORD PARSING")
print("=" * 70)

# The file likely has a header, then records
# Let's try to find a pattern in the first few hundred bytes
# Look for the first FILETIME marker — everything before it might be a file header
if ft_positions:
    first_ft = ft_positions[0]
    print(f"\nFirst FILETIME at 0x{first_ft:05X}")
    print(f"File header (before first FT, minus 8 for possible record header):")
    hdr_end = max(0, first_ft - 8)
    for row in range(0, min(hdr_end, 512), 16):
        end = min(row + 16, hdr_end)
        hexpart = ' '.join(f'{data[row+i]:02X}' for i in range(end - row))
        ascpart = ''.join(chr(data[row+i]) if 32 <= data[row+i] < 127 else '.'
                          for i in range(end - row))
        print(f"    {row:04X}: {hexpart:<48s}  {ascpart}")

    # Try parsing from just before first FILETIME
    # Record might be: [type:4][FILETIME:8][direction:4][length:4][data:length]
    print(f"\n\nTrying record parse from 0x{max(0,first_ft-16):05X}:")
    for start_off in range(max(0, first_ft - 32), first_ft, 4):
        region = data[start_off:start_off + 64]
        hexstr = ' '.join(f'{b:02X}' for b in region)
        print(f"  @ 0x{start_off:05X}: {hexstr}")

# === 8. Just show unique byte sequences that contain our EEPROM addresses ===
print("\n\n" + "=" * 70)
print("ALL '48 03' OCCURRENCES (address 0x0348 in LE)")
print("=" * 70)
for i in range(len(data) - 2):
    if data[i] == 0x48 and data[i+1] == 0x03:
        ctx = data[max(0, i-16):i+16]
        hexstr = ' '.join(f'{b:02X}' for b in ctx)
        print(f"  @ 0x{i:05X}: {hexstr}")

print("\n\n" + "=" * 70)
print("ALL '94 00' OCCURRENCES (address 0x0094 in LE)")
print("=" * 70)
count = 0
for i in range(len(data) - 2):
    if data[i] == 0x94 and data[i+1] == 0x00:
        ctx = data[max(0, i-16):i+16]
        hexstr = ' '.join(f'{b:02X}' for b in ctx)
        count += 1
        if count <= 20:
            print(f"  @ 0x{i:05X}: {hexstr}")
print(f"  Total: {count}")

print("\n\n" + "=" * 70)
print("ALL '71 01' OCCURRENCES (address 0x0171 in LE)")
print("=" * 70)
count = 0
for i in range(len(data) - 2):
    if data[i] == 0x71 and data[i+1] == 0x01:
        ctx = data[max(0, i-16):i+16]
        hexstr = ' '.join(f'{b:02X}' for b in ctx)
        count += 1
        if count <= 20:
            print(f"  @ 0x{i:05X}: {hexstr}")
print(f"  Total: {count}")
