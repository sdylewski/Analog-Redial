"""Parse EEPROM txt - show actual entries and find patterns."""
lines = open(r'c:\Users\sdylewski\Documents\BM64\DSPK+v2.1.7+Package\Software\EEPROM Table\MCHP_DSPKv2.1.7_BM64CLS1.txt').readlines()

vals = {}
for line in lines:
    line = line.strip()
    if not line or line.startswith('#') or line.startswith('//'):
        continue
    parts = line.split()
    if len(parts) >= 2:
        try:
            addr = int(parts[0], 16)
            vals[addr] = parts[1]
        except:
            pass

addrs = sorted(vals.keys())
print(f'Range: 0x{addrs[0]:04X} - 0x{addrs[-1]:04X}, {len(addrs)} entries\n')

# Print ALL entries grouped by region
print("=== Full dump (addr: value) ===")
prev = -10
for addr in addrs:
    if addr - prev > 4:
        print()  # gap
    val = vals[addr]
    marker = ""
    if addr == 0x0094: marker = "  <-- headphone DAC"
    if addr == 0x0171: marker = "  <-- key setting"
    if addr == 0x01FC: marker = "  <-- key setting"
    print(f"  0x{addr:04X}: {val:4s}{marker}")
    prev = addr

# Build full byte array (for pattern matching) using non-** entries
print("\n\n=== Consecutive byte runs (3+) ===")
non_star = {a: int(vals[a], 16) for a in addrs if vals[a] != '**'}
sorted_addrs = sorted(non_star.keys())

runs = []
run_start = sorted_addrs[0]
run_bytes = [non_star[sorted_addrs[0]]]
for i in range(1, len(sorted_addrs)):
    if sorted_addrs[i] == sorted_addrs[i-1] + 1:
        run_bytes.append(non_star[sorted_addrs[i]])
    else:
        if len(run_bytes) >= 4:
            runs.append((run_start, bytes(run_bytes)))
        run_start = sorted_addrs[i]
        run_bytes = [non_star[sorted_addrs[i]]]
if len(run_bytes) >= 4:
    runs.append((run_start, bytes(run_bytes)))

runs.sort(key=lambda x: -len(x[1]))
for start, bdata in runs[:15]:
    print(f"  0x{start:04X} ({len(bdata):3d}B): {' '.join(f'{b:02X}' for b in bdata[:20])}")
