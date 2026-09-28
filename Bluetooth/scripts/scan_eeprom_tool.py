"""Scan EEPROM_Tool.exe for HCI commands and EEPROM protocol details."""
import re

path = r'c:\Users\sdylewski\Documents\BM64\DSPK+v2.1.7+Package\Tools\EEPROM_Tool\EEPROM_Tool.exe'
data = open(path, 'rb').read()
print(f"Size: {len(data)} bytes")

# Search for HCI command patterns: 01 xx FC (vendor-specific)
print("\n=== HCI vendor command patterns (01 xx FC) ===")
hits = {}
for i in range(len(data)-3):
    if data[i] == 0x01 and data[i+2] == 0xFC:
        ocf = data[i+1]
        plen = data[i+3]
        if plen <= 32:  # reasonable param length
            ctx = data[i:i+4+min(plen,12)]
            if ocf not in hits:
                hits[ocf] = []
            hits[ocf].append((i, plen, ctx))

for ocf in sorted(hits.keys()):
    examples = hits[ocf]
    print(f"\n  FC{ocf:02X} ({len(examples)} hits):")
    for offset, plen, ctx in examples[:4]:
        print(f"    0x{offset:06X} paramLen={plen:3d}: {' '.join(f'{b:02X}' for b in ctx)}")

# Search for 16-bit LE opcode references
print("\n\n=== 16-bit LE opcode references ===")
for ocf in [0x09, 0x0A, 0x0B, 0x20, 0x29, 0x31, 0x62]:
    pattern = bytes([ocf, 0xFC])
    count = data.count(pattern)
    print(f"  FC{ocf:02X} ({ocf:02X} FC): {count} matches")

# Search for EEPROM-related strings
print("\n=== String search ===")
for pat in [b'eeprom', b'EEPROM', b'flash', b'Flash', b'sflash', b'SFlash',
            b'read_eeprom', b'write_eeprom', b'cfg_read', b'cfg_write',
            b'mem_read', b'mem_write', b'spi_read', b'spi_write',
            b'ReadCfg', b'WriteCfg', b'ReadMem', b'WriteMem',
            b'IBDK', b'ibdk', b'IB_CFG', b'ib_cfg', b'config']:
    positions = [m.start() for m in re.finditer(re.escape(pat), data)]
    if positions:
        print(f"\n  '{pat.decode()}' ({len(positions)} hits):")
        for pos in positions[:5]:
            start = max(0, pos - 8)
            end = min(len(data), pos + len(pat) + 40)
            ctx = data[start:end]
            txt = ''.join(chr(b) if 32 <= b < 127 else '.' for b in ctx)
            print(f"    0x{pos:06X}: {txt}")

# Look for IB_CFG or config flash section references
print("\n=== Looking for flash section names ===")
for pat in [b'IB_CFG', b'IB_EEPROM', b'IB_CONFIG', b'info_block', b'InfoBlock',
            b'nvm', b'NVM', b'nvram', b'NVRAM', b'param', b'PARAM',
            b'config_data', b'ConfigData', b'user_data', b'UserData']:
    positions = [m.start() for m in re.finditer(re.escape(pat), data)]
    if positions:
        print(f"  '{pat.decode()}': {len(positions)} hits")
        for pos in positions[:3]:
            ctx = data[max(0,pos-4):pos+len(pat)+20]
            txt = ''.join(chr(b) if 32 <= b < 127 else '.' for b in ctx)
            print(f"    0x{pos:06X}: {txt}")
