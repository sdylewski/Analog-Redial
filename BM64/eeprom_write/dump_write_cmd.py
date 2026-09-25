import glob

folder = r'c:\Users\sdylewski\Documents\DMS Log Files'
path = glob.glob(folder + r'\*.dmslog8')[0]
data = open(path, 'rb').read()

import sys
target = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x02313B

print("=== 150 bytes BEFORE the write record ===")
start = target - 150
region = data[start:target+140]
for i in range(0, len(region), 16):
    chunk = region[i:i+16]
    addr = start + i
    hexstr = ' '.join(f'{b:02X}' for b in chunk)
    asciistr = ''.join(chr(b) if 32<=b<127 else '.' for b in chunk)
    marker = " <== RECORD START" if addr <= target < addr+16 else ""
    print(f"  0x{addr:06X}: {hexstr:<48s} {asciistr}{marker}")

print("\n\n=== Full 128-byte data record ===")
full = data[target:target+128]
for i in range(0, len(full), 16):
    chunk = full[i:i+16]
    addr = target+i
    hexstr = ' '.join(f'{b:02X}' for b in chunk)
    print(f"  0x{addr:06X}: {hexstr}")

print("\n=== 60 bytes AFTER the record ===")
after = data[target+128:target+128+60]
for i in range(0, len(after), 16):
    chunk = after[i:i+16]
    addr = target+128+i
    hexstr = ' '.join(f'{b:02X}' for b in chunk)
    print(f"  0x{addr:06X}: {hexstr}")
