"""
Dump the full active data region of the 2026-09-25 DMS log in hex+ASCII
and decode the IOCTL-wrapped command structure.

From earlier analysis, the IOCTL blocks carry:
  READ:  ... 0A 00 05 02 [ADDR32_LE] [LEN32_LE] ...
  WRITE: ... 0B 00 05 02 [ADDR32_LE] [LEN32_LE] [DATA] ...
"""
import os, struct

LOG_PATH = os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-084134.dmslog8")

data = open(LOG_PATH, 'rb').read()

# The active region starts around 0x12800 based on earlier analysis
# Dump from first IOCTL marker area to end of file
START = 0x12800
END = len(data)

print(f"File size: {len(data)} bytes")
print(f"Dumping 0x{START:05X} to 0x{END:05X} ({END-START} bytes)\n")

# === 1. Full hex+ASCII dump of active region ===
print("=" * 100)
print("HEX + ASCII DUMP")
print("=" * 100)
for row in range(START, END, 16):
    n = min(16, END - row)
    hexpart = ' '.join(f'{data[row+i]:02X}' for i in range(n))
    ascpart = ''.join(chr(data[row+i]) if 32 <= data[row+i] < 127 else '.'
                      for i in range(n))
    # Highlight rows containing known patterns
    marker = ""
    segment = data[row:row+n]
    if b'\x48\x03' in segment:
        marker = "  <<< ADDR 0x0348"
    elif b'\x02\xFF\x0F' in segment:
        marker = "  <<< IOCTL"
    elif b'\x04\x4D\xDD\x01' in segment:
        marker = "  <<< FILETIME"
    elif b'\x08\x03' in segment and b'\x0B\x03' not in segment:
        marker = "  <<< 0803-record"
    elif b'\x0B\x03' in segment:
        marker = "  <<< 0B03-record"
    print(f"  {row:05X}: {hexpart:<48s}  {ascpart}{marker}")

# === 2. Decode the structured records ===
print("\n\n" + "=" * 100)
print("DECODED IOCTL COMMANDS (address-bearing records)")
print("=" * 100)

# Search for the pattern: [0A|0B] 00 05 02 [addr4] [len4] [optional data]
for i in range(START, END - 12):
    if data[i+1] == 0x00 and data[i+2] == 0x05 and data[i+3] == 0x02:
        cmd = data[i]
        addr = struct.unpack_from('<I', data, i+4)[0]
        length = struct.unpack_from('<I', data, i+8)[0]
        
        if cmd == 0x0A and length <= 256:  # READ
            print(f"\n  @ 0x{i:05X}: READ  addr=0x{addr:04X} len={length}")
            # Look for the response value nearby (within 200 bytes after)
            ctx = data[i:i+40]
            print(f"    context: {' '.join(f'{b:02X}' for b in ctx)}")
            
        elif cmd == 0x0B and length <= 256:  # WRITE
            write_data = data[i+12:i+12+length]
            print(f"\n  @ 0x{i:05X}: WRITE addr=0x{addr:04X} len={length} data=[{' '.join(f'{b:02X}' for b in write_data)}]")
            ctx = data[i:i+40]
            print(f"    context: {' '.join(f'{b:02X}' for b in ctx)}")

# === 3. Look at 08 03 and 0B 03 sub-records (potential serial data) ===
print("\n\n" + "=" * 100)
print("SERIAL DATA RECORDS (0x08,0x03 and 0x0B,0x03 type)")
print("=" * 100)

for i in range(START, END - 20):
    if data[i] == 0x08 and data[i+1] == 0x03 and data[i+2] == 0x00 and data[i+3] == 0x00:
        # 08 03 record — dump surrounding context
        ctx = data[i:min(i+48, END)]
        print(f"\n  0803 @ 0x{i:05X}: {' '.join(f'{b:02X}' for b in ctx)}")
        
    elif data[i] == 0x0B and data[i+1] == 0x03 and data[i+2] == 0x00 and data[i+3] == 0x00:
        ctx = data[i:min(i+48, END)]
        print(f"\n  0B03 @ 0x{i:05X}: {' '.join(f'{b:02X}' for b in ctx)}")


# === 4. Summary: extract the protocol ===
print("\n\n" + "=" * 100)
print("PROTOCOL SUMMARY")
print("=" * 100)

reads = []
writes = []
for i in range(START, END - 12):
    if data[i+1] == 0x00 and data[i+2] == 0x05 and data[i+3] == 0x02:
        cmd = data[i]
        addr = struct.unpack_from('<I', data, i+4)[0]
        length = struct.unpack_from('<I', data, i+8)[0]
        if cmd == 0x0A and length <= 256:
            reads.append((i, addr, length))
        elif cmd == 0x0B and length <= 256:
            write_data = data[i+12:i+12+length]
            writes.append((i, addr, length, write_data))

print(f"\nTotal READs:  {len(reads)}")
for off, addr, length in reads:
    print(f"  addr=0x{addr:04X} len={length}")

print(f"\nTotal WRITEs: {len(writes)}")
for off, addr, length, wdata in writes:
    print(f"  addr=0x{addr:04X} len={length} data=[{' '.join(f'{b:02X}' for b in wdata)}]")
