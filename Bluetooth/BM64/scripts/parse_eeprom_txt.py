"""Parse the EEPROM .txt file to find unique byte patterns for scanning."""
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
            val = parts[1]
            vals[addr] = val
        except:
            pass

addrs = sorted(vals.keys())
print(f'Address range: 0x{addrs[0]:04X} - 0x{addrs[-1]:04X} ({len(addrs)} entries)')
print(f'Non-** entries: {sum(1 for v in vals.values() if v != "**")}')

# Find longest consecutive run of non-** values
runs = []
cur_start = None
cur_vals = []
for addr in sorted(vals.keys()):
    v = vals[addr]
    if v != '**':
        if cur_start is None:
            cur_start = addr
            cur_vals = [v]
        elif addr == cur_start + len(cur_vals):
            cur_vals.append(v)
        else:
            if len(cur_vals) >= 3:
                runs.append((cur_start, cur_vals[:]))
            cur_start = addr
            cur_vals = [v]
    else:
        if cur_start is not None and len(cur_vals) >= 3:
            runs.append((cur_start, cur_vals[:]))
        cur_start = None
        cur_vals = []
if cur_start and len(cur_vals) >= 3:
    runs.append((cur_start, cur_vals[:]))

runs.sort(key=lambda x: -len(x[1]))
print(f'\nBest consecutive patterns for scanning:')
for start, vlist in runs[:10]:
    hex_vals = ' '.join(vlist[:16])
    print(f'  0x{start:04X} ({len(vlist):3d} bytes): {hex_vals}')
    byte_vals = bytes([int(v, 16) for v in vlist[:8]])
    print(f'         search: {" ".join(f"{b:02X}" for b in byte_vals)}')

# Also show raw dump of first 16 and key addresses
print(f'\nKey values:')
for addr in [0x0094, 0x0171, 0x01FC, 0x0348, 0x0349]:
    if addr in vals:
        print(f'  0x{addr:04X}: {vals[addr]}')
