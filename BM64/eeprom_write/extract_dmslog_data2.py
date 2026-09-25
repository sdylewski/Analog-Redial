import glob, struct, sys
sys.stdout = open('extract_output2.txt', 'w', encoding='utf-8')

folder = r'c:\Users\sdylewski\Documents\DMS Log Files'
path = glob.glob(folder + r'\*.dmslog8')[0]
data = open(path, 'rb').read()

# Parse ground-truth EEPROM table
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

def score_against_eeprom(chunk, base_addr):
    """Count matches assuming chunk[i] should equal vals[base_addr+i]."""
    matches = 0
    checked = 0
    for i, b in enumerate(chunk):
        a = base_addr + i
        if a in vals:
            checked += 1
            if b == vals[a]:
                matches += 1
    return matches, checked

# Extract records same as before, but also track large records separately
records = []
large_records = []
pos = 0
while pos < len(data) - 8:
    if data[pos:pos+4] == b'\x00\x00\x00\x00':
        length = struct.unpack_from('<I', data, pos+4)[0]
        if 1 <= length <= 500 and pos + 8 + length <= len(data):
            data_start = pos + 8
            chunk = data[data_start:data_start+length]
            records.append((data_start, chunk))
            pos = data_start + length
            continue
        elif 500 < length <= 20000 and pos + 8 + length <= len(data):
            data_start = pos + 8
            chunk = data[data_start:data_start+length]
            large_records.append((data_start, chunk, length))
            pos = data_start + length
            continue
    pos += 1

print(f"Total records: {len(records)}\n")
print(f"Total LARGE records (500-20000 bytes): {len(large_records)}\n")

# Build 4-byte sequence -> address lookup from known EEPROM values (only where all 4 consecutive known)
seq_to_addr = {}
max_addr = max(vals.keys())
for a in range(0, max_addr - 3):
    if all((a+i) in vals for i in range(4)):
        seq = bytes(vals[a+i] for i in range(4))
        seq_to_addr.setdefault(seq, []).append(a)

print(f"Built lookup with {len(seq_to_addr)} unique 4-byte sequences\n")

for off, chunk, length in large_records:
    best = (0, 0, 0, 0)
    for hdr_len in range(0, min(16, len(chunk)-4)):
        seq0 = bytes(chunk[hdr_len:hdr_len+4])
        for base in seq_to_addr.get(seq0, []):
            m, c = score_against_eeprom(chunk[hdr_len:], base)
            if c >= 8 and m > best[0]:
                best = (m, c, base, hdr_len)
    m, c, base, hdr_len = best
    ratio = (m/c) if c else 0
    print(f"  LARGE record @ file=0x{off:06X} len={length}  best_match={m}/{c} ({ratio:.0%}) base=0x{base:04X} hdr_len={hdr_len}")
    if c > 0:
        print(f"    header: {' '.join(f'{b:02X}' for b in chunk[:hdr_len])}")
        print(f"    data:   {' '.join(f'{b:02X}' for b in chunk[hdr_len:hdr_len+40])}")
print()
print("=== Records with strong EEPROM match ===\n")

best_hits = []
for idx, (off, chunk) in enumerate(records):
    if idx % 200 == 0:
        print(f"...progress: {idx}/{len(records)} records scanned", flush=True)
    if len(chunk) < 8:
        continue
    best = (0, 0, 0, 0)  # matches, checked, base, header_len
    for hdr_len in range(0, min(8, len(chunk)-4)):
        seq0 = bytes(chunk[hdr_len:hdr_len+4])
        candidates = seq_to_addr.get(seq0, [])
        for base in candidates:
            data_part = chunk[hdr_len:]
            m, c = score_against_eeprom(data_part, base)
            if c >= 4 and m > best[0]:
                best = (m, c, base, hdr_len)
    # Relaxed threshold to catch more candidates for comparison
    if best[0] >= 12 and best[1] > 0 and (best[0] / best[1]) >= 0.85:
        best_hits.append((idx, off, chunk, best))

print(f"\nDone scanning all {len(records)} records.\n", flush=True)
print(f"Found {len(best_hits)} records with EEPROM matches (>=12 bytes, >=85% match rate)\n")
# Sort by EEPROM base address to make header-vs-address patterns easier to spot
best_hits.sort(key=lambda h: h[3][2])
for idx, off, chunk, (m, c, base, hdr_len) in best_hits:
    header = chunk[:hdr_len]
    nonzero_data = sum(1 for b in chunk[hdr_len:hdr_len+60] if b != 0)
    print(f"[{idx}] file=0x{off:06X} len={len(chunk)} EEPROM_base=0x{base:04X} hdr_len={hdr_len} matched={m}/{c} nonzero_sample={nonzero_data}")
    print(f"    header: {' '.join(f'{b:02X}' for b in header)}")
    print(f"    data:   {' '.join(f'{b:02X}' for b in chunk[hdr_len:hdr_len+60])}")
