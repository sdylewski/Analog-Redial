"""
Try non-HCI protocols. isUpdate may use a proprietary framing
alongside/after the HCI handshake.

Also: try FC09 with a proper delay and flush to eliminate
response bleed, and scan with longer param formats.
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

s = serial.Serial(PORT, BAUD, timeout=3)
time.sleep(2)
s.reset_input_buffer()

def send_recv(data, wait=0.5, label=""):
    s.reset_input_buffer()
    time.sleep(0.05)  # ensure buffer is clear
    s.write(data)
    time.sleep(wait)
    resp = s.read(s.in_waiting or 200)
    tx = ' '.join(f'{b:02X}' for b in data)
    rx = ' '.join(f'{b:02X}' for b in resp) if resp else 'NO RESPONSE'
    tag = f" [{label}]" if label else ""
    print(f"  TX: {tx}{tag}")
    print(f"  RX: {rx}")
    if resp:
        ascii_str = ''.join(chr(b) if 32<=b<127 else '.' for b in resp)
        print(f"      ASCII: {ascii_str}")
    print()
    return resp

def hci_cmd(opcode, params=b'', wait=0.5):
    cmd = bytes([0x01, opcode & 0xFF, (opcode >> 8) & 0xFF, len(params)]) + params
    s.reset_input_buffer()
    time.sleep(0.05)
    s.write(cmd)
    time.sleep(wait)
    return s.read(s.in_waiting or 200)

# HCI Init
print("=== HCI Init ===")
hci_cmd(0x0C03, wait=1.5)
s.read(s.in_waiting)
hci_cmd(0xFC20, wait=0.5)
hci_cmd(0xFC62, bytes([0x01, 0x01, 0x01, 0x04]), wait=0.3)
hci_cmd(0xFC31, bytes([0x00,0x02,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x02,0x00,0x00]), wait=0.3)

# === Try ACL data packet (type 0x02) ===
print("=== HCI ACL data (type 0x02) ===")
# ACL format: 02 [handle_lo handle_hi] [len_lo len_hi] [data...]
send_recv(bytes([0x02, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10]), label="ACL read-like")

# === Try raw ISSC protocol frames ===
print("=== Raw proprietary protocols ===")

# Try 0x05 packet type (extended HCI)
send_recv(bytes([0x05, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10]), label="type=05")

# Try starting with 0x02 (STX in some protocols)
send_recv(bytes([0x02, 0x00, 0x00, 0x00, 0x10, 0x00]), label="STX frame")

# Try ISSC/AIROHA protocol: 0x01 0xFD (vendor group)
send_recv(bytes([0x01, 0xFD, 0xFC, 0x04, 0x00, 0x00, 0x00, 0x10]), label="FC FD")

# === FC09 with LONG params (5, 6, 8 bytes) ===
print("=== FC09 with long params ===")
# Maybe FC09 needs: [type, addr_lo, addr_hi, len_lo, len_hi]
for fmt_name, params in [
    ("type=0,addr=0,len=16", bytes([0x00, 0x00, 0x00, 0x10, 0x00])),
    ("type=1,addr=0,len=16", bytes([0x01, 0x00, 0x00, 0x10, 0x00])),
    ("type=2,addr=0,len=16", bytes([0x02, 0x00, 0x00, 0x10, 0x00])),
    ("6-byte:0,0,0,0,0,10",  bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x10])),
    ("8-byte:0,0,0,0,0,0,0,10", bytes([0x00]*7 + [0x10])),
    ("5-byte:type=3,addr=0,len=16", bytes([0x03, 0x00, 0x00, 0x10, 0x00])),
]:
    r = hci_cmd(0xFC09, params, wait=0.4)
    status = r[6] if r and len(r) > 6 else -1
    data = r[7:] if r and len(r) > 7 else b''
    has_bt = b'\x40\xBC\x32' in r if r else False
    print(f"  FC09({fmt_name}): status={status}, len={len(data)}, data={' '.join(f'{b:02X}' for b in data[:20])}")
    if has_bt:
        print(f"  *** BT ADDR FOUND! ***")

# === Try FC09 with type byte before addr ===
print("\n=== FC09 with type prefix byte ===")
# Type 0 = flash (default), type 1,2,3 = other memories?
for t in range(8):
    params = bytes([t, 0x00, 0x00, 0x08])
    r = hci_cmd(0xFC09, params, wait=0.4)
    if r and len(r) > 10:
        data = r[10:]
        if data != b'\x55'*len(data):
            print(f"  type={t}: {' '.join(f'{b:02X}' for b in data)}")

# === Try commands FC40-FC4F range more carefully ===
print("\n=== FC40-FC4F with various param lengths ===")
for ocf in range(0x40, 0x50):
    for plen in [0, 1, 2, 3, 4, 5, 8]:
        params = bytes([0x00] * plen)
        r = hci_cmd(0xFC00 | ocf, params, wait=0.2)
        if r and len(r) > 7 and (r[6] == 0x00 or len(r) > 10):
            data_hex = ' '.join(f'{b:02X}' for b in r)
            print(f"  FC{ocf:02X}(plen={plen}): {data_hex}")
            break

print("\nDone.")
s.close()
