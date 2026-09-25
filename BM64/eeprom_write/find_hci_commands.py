"""
Now that we KNOW the BM64 accepts HCI commands in IBDK mode:
  - 01 31 FC 0C ... → 04 0E 08 01 31 FC 00 ... (success!)
  - 01 03 0C 00     → 04 0E 04 01 03 0C 00     (success!)
  - 0A 00 05 02 ... → 04 10 01 10               (hardware error - NOT HCI!)

Find ALL HCI command patterns (01 XX XX) in the DMS capture,
especially vendor-specific ones (01 XX FC), to identify which
HCI command does EEPROM read/write.
"""
import os, struct

LOG_PATH = os.path.join(os.path.dirname(__file__),
    "USB Serial Device (COM9)$20260925-085143.dmslog8")
data = open(LOG_PATH, 'rb').read()

# Find active data region (skip file header)
active_start = 0x13500  # from previous analysis

# Search for HCI command patterns: 01 [opcode_lo] [opcode_hi] [param_len]
# where param_len is reasonable (0-255) and the full command fits
print("=" * 100)
print("ALL HCI COMMAND PATTERNS (01 XX XX LEN) in active region")
print("=" * 100)

seen_opcodes = {}
hci_cmds = []

for i in range(active_start, len(data) - 4):
    if data[i] != 0x01:
        continue
    
    opcode_lo = data[i+1]
    opcode_hi = data[i+2]
    param_len = data[i+3]
    opcode = (opcode_hi << 8) | opcode_lo
    ogf = opcode >> 10
    ocf = opcode & 0x3FF
    
    # Filter: reasonable param_len, and the full command should fit
    if param_len > 64:
        continue
    if i + 4 + param_len > len(data):
        continue
    
    # Additional filter: the byte before 01 should NOT be part of a longer sequence
    # (to avoid matching in the middle of other data)
    # Accept if preceded by 00, or a FILETIME marker, or start of region
    
    params = data[i+4:i+4+param_len]
    
    # Track unique opcodes
    if opcode not in seen_opcodes:
        seen_opcodes[opcode] = []
    seen_opcodes[opcode].append(i)
    
    hci_cmds.append((i, opcode, ogf, ocf, param_len, params))

# Show unique opcodes sorted by first occurrence
print(f"\nFound {len(hci_cmds)} potential HCI commands, {len(seen_opcodes)} unique opcodes\n")

for opcode in sorted(seen_opcodes.keys(), key=lambda k: seen_opcodes[k][0]):
    positions = seen_opcodes[opcode]
    ogf = opcode >> 10
    ocf = opcode & 0x3FF
    # Get first occurrence's params
    for cmd in hci_cmds:
        if cmd[1] == opcode:
            first_params = cmd[5]
            first_plen = cmd[4]
            break
    
    ogf_name = {0: "NOP", 1: "Link Control", 2: "Link Policy", 3: "HCI Control",
                4: "Info Params", 5: "Status", 8: "LE", 0x3F: "Vendor"}.get(ogf, f"OGF={ogf}")
    
    # Only show if appears 2+ times (DMS duplicates each command) or is vendor
    if len(positions) >= 2 or ogf == 0x3F:
        print(f"  Opcode 0x{opcode:04X} (OGF={ogf}={ogf_name}, OCF=0x{ocf:03X})")
        print(f"    Occurrences: {len(positions)} at {[f'0x{p:05X}' for p in positions[:6]]}")
        print(f"    First params ({first_plen}B): {first_params.hex()}")
        print()

# Now focus specifically on the region around EEPROM operations (0x14200 - 0x15300)
print("\n" + "=" * 100)
print("HCI COMMANDS NEAR EEPROM OPERATIONS (0x14200 - 0x15300)")
print("=" * 100)

for i, opcode, ogf, ocf, plen, params in hci_cmds:
    if 0x14200 <= i <= 0x15300:
        ogf_name = {0: "NOP", 1: "Link Control", 2: "Link Policy", 3: "HCI Control",
                    4: "Info Params", 5: "Status", 8: "LE", 0x3F: "Vendor"}.get(ogf, f"OGF={ogf}")
        print(f"  0x{i:05X}: opcode=0x{opcode:04X} ({ogf_name}) plen={plen} params={params[:20].hex()}")

# Also search for HCI events (04 XX) that might contain EEPROM data
print("\n" + "=" * 100)
print("HCI EVENTS (04 XX LEN) NEAR EEPROM OPERATIONS")
print("=" * 100)

for i in range(0x14200, min(len(data) - 3, 0x15300)):
    if data[i] == 0x04:
        evt_code = data[i+1]
        evt_len = data[i+2]
        if evt_len <= 64 and i + 3 + evt_len <= len(data):
            evt_data = data[i+3:i+3+evt_len]
            # Only show Command Complete (0x0E) and vendor events
            if evt_code in (0x0E, 0x0F, 0xFF, 0x10):
                print(f"  0x{i:05X}: event=0x{evt_code:02X} len={evt_len} data={evt_data.hex()}")

# Search specifically for the known response patterns
print("\n" + "=" * 100)
print("SEARCH FOR EEPROM DATA VALUE 0x0A NEAR READ OPERATIONS")
print("=" * 100)

# The IOCTL_READ operations are at 0x14331 etc.
# The expected EEPROM value at 0x0348 is 0x0A
# Look for HCI Command Complete events that might contain 0x0A as data

for i in range(0x14200, min(len(data) - 20, 0x15500)):
    # Look for Command Complete for vendor opcodes: 04 0E XX 01 XX FC ...
    if (data[i] == 0x04 and data[i+1] == 0x0E and 
        i + 3 + data[i+2] <= len(data) and data[i+2] >= 4):
        evt_len = data[i+2]
        num_cmds = data[i+3]
        opcode = (data[i+5] << 8) | data[i+4]
        status = data[i+6] if evt_len > 3 else -1
        ogf = opcode >> 10
        if ogf == 0x3F:  # Vendor
            full_data = data[i:i+3+evt_len]
            print(f"  0x{i:05X}: CmdComplete opcode=0x{opcode:04X} status={status} data={full_data.hex()}")
