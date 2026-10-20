"""
BM64 HCI probe — using the REAL opcodes found in isupdate.exe!

isupdate.exe connect sequence (decoded from x86 MOV instructions):
  1. FC20 (no params)     — init/reset
  2. FC62 (01 01 01 04)   — device identify
  3. FC31 (00 02 00 00 00 00 00 00 00 02 00 00) — config
  4. FCA4 (00 03 01 9F)   — version/status read

FCA4 is the multi-purpose command for memory operations.
First byte of params appears to be sub-command type.

Run with the working bridge (bm64_bridge_arduino, BOOT_MODE 0).
Close Arduino IDE first. May need 2nd try after power cycle.

Usage: python -u probe_real_hci.py
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

def open_port():
    try:
        s = serial.Serial(PORT, BAUD, timeout=3)
        time.sleep(2)
        s.reset_input_buffer()
        return s
    except serial.SerialException as e:
        print(f"FAILED: {e}")
        sys.exit(1)

def hci_cmd(s, opcode, params=b'', wait=1.0, label=""):
    cmd = bytes([0x01, opcode & 0xFF, (opcode >> 8) & 0xFF, len(params)]) + params
    s.reset_input_buffer()
    s.write(cmd)
    time.sleep(wait)
    resp = s.read(s.in_waiting or 200)
    
    cmd_hex = ' '.join(f'{b:02X}' for b in cmd)
    resp_hex = ' '.join(f'{b:02X}' for b in resp) if resp else 'NO RESPONSE'
    tag = f" [{label}]" if label else ""
    print(f"  TX: {cmd_hex}{tag}")
    print(f"  RX: {resp_hex}")
    
    # Check for Command Complete
    if resp and len(resp) >= 6 and resp[0] == 0x04 and resp[1] == 0x0E:
        status = resp[6] if len(resp) > 6 else -1
        op_echo = resp[4] | (resp[5] << 8) if len(resp) > 5 else 0
        print(f"      CmdComplete for 0x{op_echo:04X}, status={status}")
        if len(resp) > 7:
            data = resp[7:]
            print(f"      Data: {' '.join(f'{b:02X}' for b in data)}")
    print()
    return resp

print("Opening port...")
s = open_port()

# === Phase 1: Replicate isupdate connect sequence ===
print("=" * 60)
print("PHASE 1: Connect sequence (from isupdate.exe)")
print("=" * 60)

# Standard HCI Reset first
hci_cmd(s, 0x0C03, wait=1.5, label="HCI_Reset")

# FC20 — init (no params)
hci_cmd(s, 0xFC20, wait=1.0, label="FC20 init")

# FC62 — identify (params: 01 01 01 04)
hci_cmd(s, 0xFC62, bytes([0x01, 0x01, 0x01, 0x04]), wait=1.0, label="FC62 identify")

# FCA4 — version read (params: 00 03 01 9F)
hci_cmd(s, 0xFCA4, bytes([0x00, 0x03, 0x01, 0x9F]), wait=1.0, label="FCA4 version")

# FC31 — config (12 bytes)
hci_cmd(s, 0xFC31, bytes([0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                           0x00, 0x02, 0x00, 0x00]), wait=1.0, label="FC31 config")

# === Phase 2: Try FCA4 sub-commands for memory access ===
print("=" * 60)
print("PHASE 2: FCA4 sub-commands (memory operations)")
print("=" * 60)

# Known FCA4 param patterns from binary analysis:
# 07 00 04 20  — possible flash read setup
# 03 00 04 20  — possible eeprom read setup  
# 07 01 01 05  — possible read command
# 07 00 02 01  — possible something
# 00 03 01 9F  — version (already tried above)

# Try reading memory with various FCA4 sub-commands
fca4_patterns = [
    (bytes([0x07, 0x00, 0x04, 0x20]), "07 00 04 20"),
    (bytes([0x07, 0x00, 0x04, 0xD8]), "07 00 04 D8"),
    (bytes([0x03, 0x00, 0x04, 0x20]), "03 00 04 20"),
    (bytes([0x03, 0x00, 0x04, 0xD8]), "03 00 04 D8"),
    (bytes([0x07, 0x01, 0x01, 0x05]), "07 01 01 05"),
    (bytes([0x07, 0x00, 0x02, 0x01]), "07 00 02 01"),
    (bytes([0x01, 0x06, 0x02, 0x72]), "01 06 02 72"),
    (bytes([0x07, 0x01, 0x02, 0x05]), "07 01 02 05"),
    (bytes([0x00, 0x03, 0x01, 0x9F]), "version read"),
]

for params, label in fca4_patterns:
    hci_cmd(s, 0xFCA4, params, wait=0.5, label=f"FCA4 {label}")

# === Phase 3: Try FCA4 with memory addresses (EEPROM at 0x0000) ===
print("=" * 60)
print("PHASE 3: FCA4 with address params (trying to read EEPROM)")
print("=" * 60)

# Maybe the pattern is: [sub_cmd, addr_lo, addr_hi, length, ...]
# Try sub_cmd 03 (possible eeprom) with addr 0x0000
for sub in [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07]:
    params = bytes([sub, 0x00, 0x00, 0x10])  # addr=0, len=16
    hci_cmd(s, 0xFCA4, params, wait=0.5, label=f"FCA4 sub={sub:02X} addr=0 len=16")

# === Phase 4: Try FC5D and FC7E ===
print("=" * 60)
print("PHASE 4: Other opcodes (FC5D, FC7E)")
print("=" * 60)

# FC5D with 4-byte params
hci_cmd(s, 0xFC5D, bytes([0x00, 0x00, 0x00, 0x10]), wait=0.5, label="FC5D")
hci_cmd(s, 0xFC5D, bytes([0x01, 0x00, 0x00, 0x10]), wait=0.5, label="FC5D type=1")

# FC7E — might be bulk read/write
hci_cmd(s, 0xFC7E, bytes([0x00, 0x00, 0x00, 0x00, 0x10, 0x00]), wait=0.5, label="FC7E 6-byte")
hci_cmd(s, 0xFC7E, bytes([0x00, 0x01, 0x02, 0x00, 0x00, 0x10, 0x00, 0x00]), wait=0.5, label="FC7E 8-byte")
hci_cmd(s, 0xFC7E, bytes([0x00, 0x01, 0x02, 0x80, 0x00, 0x10, 0x00, 0x00]), wait=0.5, label="FC7E type=80")

print("Done.")
s.close()
