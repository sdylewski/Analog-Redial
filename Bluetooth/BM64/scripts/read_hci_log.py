"""
Read the HCI traffic log from the logging bridge WITHOUT triggering
a DTR reset (which would reboot the ESP32 and lose the buffer).

Run this AFTER isUpdate is closed and 15s have passed.
The bridge sketch seals the log and waits for a trigger byte.

Usage: python read_hci_log.py
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200
TAG_PC   = 0xAA
TAG_BM64 = 0xBB

# Open port WITHOUT asserting DTR (prevents ESP32 reset)
s = serial.Serial()
s.port = PORT
s.baudrate = BAUD
s.timeout = 5
s.dtr = False    # critical: don't toggle DTR
s.rts = False    # don't toggle RTS either
try:
    s.open()
except serial.SerialException as e:
    print(f"Cannot open {PORT}: {e}")
    print("Make sure isUpdate and Arduino IDE are closed!")
    sys.exit(1)

time.sleep(0.5)
s.reset_input_buffer()

# Send trigger byte to request the dump
print(f"Sending trigger to {PORT}...")
s.write(b'D')
time.sleep(1)

# Read the response
raw = b''
deadline = time.time() + 10
while time.time() < deadline:
    chunk = s.read(s.in_waiting or 1)
    if chunk:
        raw += chunk
        deadline = time.time() + 2  # extend deadline while data flows
    else:
        if raw:
            break

s.close()

if not raw:
    print("No response. Is the logging bridge running and sealed?")
    print("(Wait 15s after closing isUpdate before running this)")
    sys.exit(1)

# Parse the response
text = raw.decode('ascii', errors='replace')
if 'LOG:' not in text:
    print(f"Unexpected response ({len(raw)} bytes):")
    print(text[:500])
    sys.exit(1)

# Extract binary log data
header_end = text.index('\n') + 1
log_len = int(text[text.index('LOG:') + 4 : text.index('\n')])
log_data = raw[header_end : header_end + log_len]

print(f"\n=== Captured HCI Traffic: {len(log_data)} bytes ===\n")

# Decode the log
i = 0
pkt = 0
while i + 3 <= len(log_data):
    tag = log_data[i]; i += 1
    plen = log_data[i] | (log_data[i+1] << 8); i += 2
    if i + plen > len(log_data):
        break

    pkt += 1
    direction = "PC->BM64" if tag == TAG_PC else "BM64->PC" if tag == TAG_BM64 else "???"
    chunk = log_data[i:i+plen]
    hex_str = ' '.join(f'{b:02X}' for b in chunk)
    print(f"[{pkt:3d}] {direction} ({plen} bytes): {hex_str}")

    # Decode HCI command
    if tag == TAG_PC and plen >= 4 and chunk[0] == 0x01:
        opcode = chunk[1] | (chunk[2] << 8)
        param_len = chunk[3]
        params = ' '.join(f'{b:02X}' for b in chunk[4:4+param_len])
        print(f"       HCI Cmd opcode=0x{opcode:04X} paramLen={param_len} params: {params}")

    # Decode HCI event
    if tag == TAG_BM64 and plen >= 3 and chunk[0] == 0x04:
        evt_code = chunk[1]
        evt_len = chunk[2]
        extra = ""
        if evt_code == 0x0E and plen >= 6:
            op = chunk[4] | (chunk[5] << 8)
            extra = f" (CmdComplete for 0x{op:04X})"
        print(f"       HCI Evt code=0x{evt_code:02X} len={evt_len}{extra}")

    i += plen

print(f"\n=== End of capture ({pkt} packets) ===")
