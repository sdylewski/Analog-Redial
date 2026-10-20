"""
Find the RESPONSE value for each READ command in both captures.

Confirmed request format (from analyze_read_capture4/5.py):
  READ:  0A 00 05 02 [ADDR_LO ADDR_HI 00 00] [LEN_LO LEN_HI 00 00]
  WRITE: 0B 00 05 02 [ADDR_LO ADDR_HI 00 00] [LEN_LO LEN_HI 00 00] [DATA...]

The response to a READ should be a separate ReadFile() call. Since this DMS
format doesn't clearly separate TX/RX at the byte level (the 0803/0B03
tagging was noise), search bytes AFTER each read-request offset for the
plausible response echo: many simple protocols echo the request back with
the data appended, i.e.:
  0A 00 05 02 [ADDR] [LEN] [DATA]   <- response, len same as request but +1 byte
"""
import os, struct

FILES = [
    "USB Serial Device (COM9)$20260925-084134.dmslog8",
    "USB Serial Device (COM9)$20260925-085143.dmslog8",
]

for fname in FILES:
    path = os.path.join(os.path.dirname(__file__), fname)
    data = open(path, 'rb').read()
    print("=" * 100)
    print(f"FILE: {fname}  ({len(data)} bytes)")
    print("=" * 100)

    # Find all READ/WRITE commands as before
    cmds = []
    for i in range(len(data) - 12):
        if data[i+1] == 0x00 and data[i+2] == 0x05 and data[i+3] == 0x02:
            cmd = data[i]
            addr = struct.unpack_from('<I', data, i+4)[0]
            length = struct.unpack_from('<I', data, i+8)[0]
            if cmd == 0x0A and length <= 16 and addr <= 0x2000:
                cmds.append((i, 'READ', addr, length, None))
            elif cmd == 0x0B and length <= 16 and addr <= 0x2000:
                wdata = data[i+12:i+12+length]
                cmds.append((i, 'WRITE', addr, length, wdata))

    print(f"\nFound {len(cmds)} commands\n")

    # For each READ command, look for a response pattern in the next 100 bytes:
    # response = 0A 00 05 02 [same addr] [same len] [DATA_BYTE]
    for i, (off, ctype, addr, length, wdata) in enumerate(cmds):
        print(f"\n[{i}] 0x{off:05X} {ctype} addr=0x{addr:04X} len={length}", end="")
        if ctype == 'WRITE':
            print(f" data={' '.join(f'{b:02X}' for b in wdata)}")
        else:
            print()

        if ctype == 'READ':
            # Search forward up to 300 bytes for the same signature with 1 extra byte
            search_start = off + 12
            search_end = min(off + 400, len(data) - 13)
            found = False
            for j in range(search_start, search_end):
                if (data[j] == 0x0A and data[j+1] == 0x00 and data[j+2] == 0x05
                        and data[j+3] == 0x02):
                    a2 = struct.unpack_from('<I', data, j+4)[0]
                    l2 = struct.unpack_from('<I', data, j+8)[0]
                    if a2 == addr and l2 == length:
                        # This could be a duplicate request (logged twice) OR
                        # the response. Check the byte right after the 12-byte
                        # struct — if it's a plausible EEPROM value, flag it.
                        maybe_val = data[j+12]
                        print(f"      -> echo/response candidate @ 0x{j:05X}, "
                              f"byte[12]=0x{maybe_val:02X}  "
                              f"(offset +{j - off} from request)")
                        found = True
            if not found:
                print(f"      -> no echo found in next 400 bytes")

    print()
