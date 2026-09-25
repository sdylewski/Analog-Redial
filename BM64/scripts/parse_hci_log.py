"""
Parse hci_log.txt captured by hci_mitm.py.
Reconstructs HCI packets from raw bytes and identifies EEPROM read/write commands.
"""
import sys
import re

LOG_FILE = 'hci_log.txt'

# Accumulate bytes per direction to handle fragmented HCI packets
tx_buf = bytearray()  # PC -> BM64
rx_buf = bytearray()  # BM64 -> PC

def parse_packets(buf, direction):
    """Extract complete HCI packets from buffer. Return (packets, remaining)."""
    packets = []
    i = 0
    while i < len(buf):
        if direction == 'TX':
            # HCI Command: 01 [opcode_lo] [opcode_hi] [plen] [params...]
            if buf[i] == 0x01 and i + 3 < len(buf):
                plen = buf[i+3]
                total = 4 + plen
                if i + total <= len(buf):
                    packets.append(('CMD', bytes(buf[i:i+total])))
                    i += total
                    continue
                else:
                    break  # incomplete
            # HCI ACL: 02 [handle_lo] [handle_hi] [len_lo] [len_hi] [data...]
            elif buf[i] == 0x02 and i + 4 < len(buf):
                dlen = buf[i+3] | (buf[i+4] << 8)
                total = 5 + dlen
                if i + total <= len(buf):
                    packets.append(('ACL', bytes(buf[i:i+total])))
                    i += total
                    continue
                else:
                    break
            else:
                # Unknown byte - skip
                packets.append(('RAW', bytes([buf[i]])))
                i += 1
        else:
            # HCI Event: 04 [evt_code] [plen] [params...]
            if buf[i] == 0x04 and i + 2 < len(buf):
                plen = buf[i+2]
                total = 3 + plen
                if i + total <= len(buf):
                    packets.append(('EVT', bytes(buf[i:i+total])))
                    i += total
                    continue
                else:
                    break
            # HCI ACL
            elif buf[i] == 0x02 and i + 4 < len(buf):
                dlen = buf[i+3] | (buf[i+4] << 8)
                total = 5 + dlen
                if i + total <= len(buf):
                    packets.append(('ACL', bytes(buf[i:i+total])))
                    i += total
                    continue
                else:
                    break
            else:
                packets.append(('RAW', bytes([buf[i]])))
                i += 1
    return packets, buf[i:]

def format_packet(ptype, data):
    """Human-readable packet description."""
    hexstr = ' '.join(f'{b:02X}' for b in data)
    
    if ptype == 'CMD' and len(data) >= 4:
        opcode = data[1] | (data[2] << 8)
        plen = data[3]
        params = data[4:]
        ogf = (opcode >> 10) & 0x3F
        ocf = opcode & 0x3FF
        params_hex = ' '.join(f'{b:02X}' for b in params)
        # Try ASCII
        ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in params)
        return f"CMD OGF={ogf:02X} OCF={ocf:03X} (0x{opcode:04X}) plen={plen} params=[{params_hex}] ascii=\"{ascii_str}\""
    
    elif ptype == 'EVT' and len(data) >= 3:
        evt = data[1]
        plen = data[2]
        params = data[3:]
        
        if evt == 0x0E and len(params) >= 3:
            opcode = params[1] | (params[2] << 8)
            status = params[3] if len(params) > 3 else -1
            rest = params[4:]
            rest_hex = ' '.join(f'{b:02X}' for b in rest)
            ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in rest)
            return f"EVT CmdComplete opcode=0x{opcode:04X} status={status} data=[{rest_hex}] ascii=\"{ascii_str}\""
        else:
            params_hex = ' '.join(f'{b:02X}' for b in params)
            return f"EVT code=0x{evt:02X} plen={plen} [{params_hex}]"
    
    elif ptype == 'ACL':
        return f"ACL [{hexstr}]"
    
    return f"RAW [{hexstr}]"

def main():
    fname = sys.argv[1] if len(sys.argv) > 1 else LOG_FILE
    
    print(f"Parsing {fname}...\n")
    
    tx_buf = bytearray()
    rx_buf = bytearray()
    
    all_packets = []
    
    with open(fname) as f:
        for line in f:
            m = re.match(r'\[\s*([\d.]+)\]\s+(TX|RX)\s+\(\s*\d+\):\s+([\dA-Fa-f ]+)', line)
            if not m:
                continue
            
            timestamp = float(m.group(1))
            direction = m.group(2)
            hex_bytes = bytes.fromhex(m.group(3).replace(' ', ''))
            
            if direction == 'TX':
                tx_buf.extend(hex_bytes)
                packets, tx_buf = parse_packets(tx_buf, 'TX')
                for ptype, pdata in packets:
                    all_packets.append((timestamp, 'TX', ptype, pdata))
            else:
                rx_buf.extend(hex_bytes)
                packets, rx_buf = parse_packets(rx_buf, 'RX')
                for ptype, pdata in packets:
                    all_packets.append((timestamp, 'RX', ptype, pdata))
    
    # Print all packets
    print(f"{'Time':>10s}  {'Dir':2s}  Packet")
    print("=" * 120)
    
    new_opcodes = set()
    for timestamp, direction, ptype, pdata in all_packets:
        desc = format_packet(ptype, pdata)
        print(f"{timestamp:10.3f}  {direction:2s}  {desc}")
        
        if ptype == 'CMD':
            opcode = pdata[1] | (pdata[2] << 8)
            new_opcodes.add(opcode)
    
    print(f"\n\n=== Summary ===")
    print(f"Total packets: {len(all_packets)}")
    print(f"TX (PC->BM64): {sum(1 for _,d,_,_ in all_packets if d=='TX')}")
    print(f"RX (BM64->PC): {sum(1 for _,d,_,_ in all_packets if d=='RX')}")
    print(f"Unique opcodes used: {', '.join(f'0x{o:04X}' for o in sorted(new_opcodes))}")
    
    # Identify potential EEPROM commands (unknown opcodes)
    known = {0x0C03, 0xFC09, 0xFC0A, 0xFC0B, 0xFC20, 0xFC29, 0xFC31, 0xFC62, 0xFCA4}
    unknown = new_opcodes - known
    if unknown:
        print(f"\n*** NEW/UNKNOWN opcodes: {', '.join(f'0x{o:04X}' for o in sorted(unknown))} ***")
        print("These are likely the EEPROM read/write commands!")

if __name__ == '__main__':
    main()
