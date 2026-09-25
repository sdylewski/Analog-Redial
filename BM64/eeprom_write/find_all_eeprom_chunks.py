import glob, sys
sys.stdout = open('all_chunks_output.txt', 'w', encoding='utf-8')

folder = r'c:\Users\sdylewski\Documents\DMS Log Files'
path = glob.glob(folder + r'\*.dmslog8')[0]
data = open(path, 'rb').read()

lines = open(r'c:\Users\sdylewski\Documents\BM64\DSPK+v2.1.7+Package\Software\EEPROM Table\MCHP_DSPKv2.1.7_BM64CLS1.txt').readlines()
vals = {}
for line in lines:
    line = line.strip()
    if not line or line.startswith(';'):
        continue
    parts = line.replace(';','').split()
    if len(parts) < 2:
        continue
    try:
        addr = int(parts[0], 16)
    except:
        continue
    for i, v in enumerate(parts[1:17]):
        if v == '**':
            continue
        try:
            vals[addr+i] = int(v, 16)
        except:
            pass

max_addr = max(vals.keys())

# Build 6-byte sequence -> address lookup (more specific, fewer false positives)
seq_to_addr = {}
for a in range(0, max_addr - 5):
    if all((a+i) in vals for i in range(6)):
        seq = bytes(vals[a+i] for i in range(6))
        seq_to_addr.setdefault(seq, []).append(a)

print(f"Built lookup with {len(seq_to_addr)} unique 6-byte sequences", flush=True)

def extend_match(file_pos, eeprom_addr, max_len=150):
    """Count consecutive matching bytes (allow occasional mismatch up to 2).
    Returns (total_length, checked_count, matched_count, nonzero_count)."""
    length = 0
    mismatches = 0
    checked = 0
    matched = 0
    nonzero = 0
    for i in range(max_len):
        fp = file_pos + i
        ea = eeprom_addr + i
        if fp >= len(data):
            break
        if ea not in vals:
            length += 1
            continue
        checked += 1
        if vals[ea] != 0:
            nonzero += 1
        if data[fp] == vals[ea]:
            matched += 1
            length += 1
        else:
            mismatches += 1
            if mismatches > 2:
                break
            length += 1
    return length, checked, matched, nonzero

hits = []
pos = 0
n = len(data)
while pos < n - 6:
    seq = data[pos:pos+6]
    candidates = seq_to_addr.get(seq)
    if candidates:
        best = None
        for base in candidates:
            length, checked, matched, nonzero = extend_match(pos, base)
            if checked >= 40 and matched == checked and nonzero >= 20:
                if best is None or length > best[0]:
                    best = (length, base, checked, matched, nonzero)
        if best:
            length, base, checked, matched, nonzero = best
            hits.append((pos, base, length, checked, matched, nonzero))
            pos += max(length - 1, 1)
            continue
    pos += 1

print(f"\nFound {len(hits)} HIGH CONFIDENCE chunks (all known bytes matched, min 15 known, min 8 nonzero)\n")
for file_pos, eeprom_base, length, checked, matched, nonzero in hits:
    header = data[max(0,file_pos-12):file_pos]
    print(f"file=0x{file_pos:06X}  eeprom_addr=0x{eeprom_base:04X}  len={length}  checked={checked} nonzero={nonzero}")
    print(f"    header(12B before): {' '.join(f'{b:02X}' for b in header)}")
