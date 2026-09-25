"""
DMS log format reverse-engineered structure:
  Real data record: [00 00 00 00][LEN:4 LE][DATA: LEN bytes]
  Followed by 1-2 "status/timestamp" noise records to skip:
    Type A (24 bytes): [LEN2:4][FLAG=0x00000001][SEQ:4][RESERVED:4][TIMESTAMP:8]
    Type B (16 bytes): [LEN2:4][FLAG=0x80000001][TIMESTAMP:8]
  These noise records' LEN2 field does NOT correspond to actual serial data.

Strategy: scan for "00 00 00 00" followed by plausible LEN (1-500), extract LEN
bytes as real data, then skip forward past known noise record shapes until next
"00 00 00 00" + LEN marker.
"""
import glob, struct

folder = r'c:\Users\sdylewski\Documents\DMS Log Files'
path = glob.glob(folder + r'\*.dmslog8')[0]
data = open(path, 'rb').read()
print(f"File size: {len(data)}\n")

def is_noise_record(pos):
    """Check if a 16 or 24-byte noise record starts at pos. Return its size or 0."""
    if pos + 16 > len(data):
        return 0
    flag = struct.unpack_from('<I', data, pos+4)[0]
    # Type B: 16 bytes, flag low byte=01, high byte=80
    if (flag & 0xFF) == 0x01 and (flag >> 24) == 0x80:
        return 16
    # Type A: 24 bytes, flag low byte=01, high byte=00
    if pos + 24 <= len(data):
        if (flag & 0xFF) == 0x01 and (flag >> 24) == 0x00:
            return 24
    return 0

records = []  # (file_offset, data_bytes)
pos = 0
while pos < len(data) - 8:
    # Look for "00 00 00 00" + plausible LEN
    if data[pos:pos+4] == b'\x00\x00\x00\x00':
        length = struct.unpack_from('<I', data, pos+4)[0]
        if 1 <= length <= 500 and pos + 8 + length <= len(data):
            data_start = pos + 8
            chunk = data[data_start:data_start+length]
            records.append((data_start, chunk))
            pos = data_start + length
            continue
    pos += 1

print(f"Extracted {len(records)} candidate data records\n")

# Print all records (hex + parsed HCI if applicable)
def parse_hci(d):
    if not d:
        return ""
    if d[0] == 0x01 and len(d) >= 4:
        opcode = d[1] | (d[2] << 8)
        plen = d[3]
        params = d[4:4+plen]
        return f"CMD 0x{opcode:04X} plen={plen} params=[{' '.join(f'{b:02X}' for b in params)}]"
    elif d[0] == 0x04 and len(d) >= 3:
        evt = d[1]
        plen = d[2]
        params = d[3:3+plen]
        if evt == 0x0E and len(params) >= 3:
            opcode = params[1] | (params[2] << 8)
            status = params[3] if len(params) > 3 else None
            rest = params[4:]
            return f"EVT CmdComplete opcode=0x{opcode:04X} status={status} data=[{' '.join(f'{b:02X}' for b in rest)}]"
        return f"EVT code=0x{evt:02X} plen={plen} [{' '.join(f'{b:02X}' for b in params)}]"
    return f"RAW[{' '.join(f'{b:02X}' for b in d[:40])}]" + ("..." if len(d)>40 else "")

for i, (off, chunk) in enumerate(records):
    desc = parse_hci(chunk)
    print(f"[{i:4d}] file=0x{off:06X} len={len(chunk):3d}: {desc}")

print(f"\n\nTotal records: {len(records)}")
