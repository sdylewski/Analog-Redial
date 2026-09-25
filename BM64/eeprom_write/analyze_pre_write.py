"""
Extract ALL [00 00 00 00][LEN][data] records from the DMS log using the
exact two-tier parsing from extract_dmslog_data2.py (confirmed: only 92
total records in this file, not the "1954" figure from an earlier session's
notes -- that number was wrong), then print every one of them chronologically
by file offset, with the confirmed real EEPROM write block marked, so the
"initial connection / mode select" commands preceding it are visible in
full context.
"""
import struct

LOG_PATH = "USB Serial Device (COM9)$20260924-160549.dmslog8"
WRITE_SIG = bytes([0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04,
                    0x04, 0x24, 0x00, 0x0A, 0x16, 0x00, 0xB8, 0x06])

def parse_records(data):
    records = []
    pos = 0
    while pos < len(data) - 8:
        if data[pos:pos+4] == b'\x00\x00\x00\x00':
            length = struct.unpack_from('<I', data, pos+4)[0]
            if 1 <= length <= 20000 and pos + 8 + length <= len(data):
                data_start = pos + 8
                chunk = data[data_start:data_start+length]
                records.append((data_start, chunk))
                pos = data_start + length
                continue
        pos += 1
    return records

def main():
    data = open(LOG_PATH, "rb").read()
    records = parse_records(data)
    print(f"Total records: {len(records)} (file size {len(data)} bytes)\n")

    write_idx = None
    for idx, (off, chunk) in enumerate(records):
        if chunk.startswith(WRITE_SIG):
            write_idx = idx
            break

    for idx, (off, chunk) in enumerate(records):
        marker = "  <<<< CONFIRMED EEPROM WRITE BLOCK" if idx == write_idx else ""
        preview = chunk[:80]
        hexstr = ' '.join(f'{b:02X}' for b in preview)
        more = f' ... ({len(chunk)} bytes total)' if len(chunk) > 80 else f' ({len(chunk)} bytes)'
        print(f"[{idx:3d}] file=0x{off:06X}{marker}")
        print(f"      {hexstr}{more}")

if __name__ == '__main__':
    main()
