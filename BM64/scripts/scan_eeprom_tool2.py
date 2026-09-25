"""Deep dive into EEPROM_Tool.exe — find the readEEPROM/writeEEPROM protocol."""
import re

path = r'c:\Users\sdylewski\Documents\BM64\DSPK+v2.1.7+Package\Tools\EEPROM_Tool\EEPROM_Tool.exe'
data = open(path, 'rb').read()

# Find all strings near readEEPROM / writeEEPROM
print("=== Strings near EEPROM functions ===")
for pat in [b'readEEPROM', b'writeEEPROM', b'ReadEEPROM', b'WriteEEPROM',
            b'read_eeprom', b'write_eeprom', b'readXMem', b'writeXMem',
            b'readRf', b'writeRf', b'EEP addr', b'eep_addr',
            b'ib_eflash', b'eflash_read', b'eflash_write',
            b'ibdk_load', b'ibdk_read', b'ibdk_write']:
    for m in re.finditer(re.escape(pat), data):
        pos = m.start()
        # Get the full string (find null terminators)
        start = pos
        while start > 0 and data[start-1] >= 0x20 and data[start-1] < 0x7F:
            start -= 1
        end = pos
        while end < len(data) and data[end] >= 0x20 and data[end] < 0x7F:
            end += 1
        string = data[start:end].decode('ascii', errors='replace')
        if len(string) > 5:
            print(f"  0x{start:06X}: {string[:120]}")

# Search for command structure patterns near EEPROM strings
print("\n\n=== Code near readEEPROM (0x2AD950 area) ===")
# Look for FC opcode references near the readEEPROM string references
# The string is at 0x2AD950. Code referencing this string would load its address.
# Search for the LE bytes of 0x2AD950 (in virtual address form)
# Need to account for PE base address (typically 0x400000 for 32-bit)
for base_va in [0x400000, 0x0]:
    va = base_va + 0x2AD950
    va_bytes = va.to_bytes(4, 'little')
    refs = [m.start() for m in re.finditer(re.escape(va_bytes), data)]
    if refs:
        print(f"  References to readEEPROM string (VA=0x{va:08X}):")
        for ref in refs[:5]:
            ctx = data[max(0,ref-16):ref+20]
            print(f"    0x{ref:06X}: {' '.join(f'{b:02X}' for b in ctx)}")

# Look at the broader area around EEPROM strings
print("\n=== All strings in 0x2AD800-0x2AE400 range ===")
region = data[0x2AD800:0x2AE400]
# Extract null-terminated strings
i = 0
while i < len(region):
    if region[i] >= 0x20 and region[i] < 0x7F:
        end = i
        while end < len(region) and region[end] >= 0x20 and region[end] < 0x7F:
            end += 1
        s = region[i:end].decode('ascii', errors='replace')
        if len(s) > 8:  # only show meaningful strings
            print(f"  0x{0x2AD800+i:06X}: {s[:150]}")
        i = end + 1
    else:
        i += 1

# Look for the script command format
print("\n=== Strings near ibdk_load_script ===")
region = data[0x2AE200:0x2AE500]
i = 0
while i < len(region):
    if region[i] >= 0x20 and region[i] < 0x7F:
        end = i
        while end < len(region) and region[end] >= 0x20 and region[end] < 0x7F:
            end += 1
        s = region[i:end].decode('ascii', errors='replace')
        if len(s) > 5:
            print(f"  0x{0x2AE200+i:06X}: {s[:150]}")
        i = end + 1
    else:
        i += 1

# Search for "writeXMem" context — this is likely the EEPROM write
print("\n=== writeXMem and readXMem patterns ===")
for pat in [b'writeXMem', b'readXMem', b'XMem', b'xmem']:
    for m in re.finditer(re.escape(pat), data):
        pos = m.start()
        start = max(0, pos - 40)
        end = min(len(data), pos + 80)
        region = data[start:end]
        txt = ''.join(chr(b) if 32 <= b < 127 else '.' for b in region)
        print(f"  0x{pos:06X}: {txt}")
