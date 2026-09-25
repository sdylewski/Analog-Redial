import serial, time, sys

PORT = 'COM9'
BAUD = 115200

print(f"Opening {PORT} at {BAUD}...")
try:
    s = serial.Serial(PORT, BAUD, timeout=3)
except serial.SerialException as e:
    print(f"FAILED to open {PORT}: {e}")
    print("Make sure PuTTY / Serial Monitor is closed!")
    sys.exit(1)

time.sleep(0.5)

# Drain any startup output
startup = s.read(s.in_waiting or 1)
if startup:
    print(f"Startup ({len(startup)} bytes): {startup.decode(errors='replace')}")

# Enter bridge mode
print("\nSending 'bridge' command...")
s.write(b'bridge\r\n')
time.sleep(3)  # Give BM64 time to enter IBDK mode

bridge_resp = s.read(s.in_waiting or 1)
if bridge_resp:
    print(f"Bridge response: {bridge_resp.decode(errors='replace')}")
else:
    print("No bridge response text (might be OK if already consumed)")

# Now test: send bytes and look for ANY response from BM64
print("\n--- Testing raw forwarding to BM64 ---")
test_bytes = [0x00, 0x01, 0x02, 0x05, 0x10, 0x55, 0xAA, 0xFF]
for b in test_bytes:
    s.write(bytes([b]))
    time.sleep(0.3)
    resp = s.read(s.in_waiting or 100)
    status = ' '.join(f'{x:02X}' for x in resp) if resp else 'EMPTY'
    print(f"  Sent 0x{b:02X} -> {status}")

# Also try a multi-byte sequence (the EEPROM_Tool might send a longer handshake)
print("\n--- Trying multi-byte sequences ---")
sequences = [
    ("3x NULL",           bytes([0x00, 0x00, 0x00])),
    ("0x01 0x00",         bytes([0x01, 0x00])),
    ("UART ver (normal)", bytes([0xAA, 0x00, 0x02, 0x08, 0x00, 0xF6])),
    ("0x55 0x55 0x55",    bytes([0x55, 0x55, 0x55])),
]
for name, seq in sequences:
    s.reset_input_buffer()
    s.write(seq)
    time.sleep(1)
    resp = s.read(s.in_waiting or 200)
    status = ' '.join(f'{x:02X}' for x in resp) if resp else 'EMPTY'
    print(f"  {name:25s} -> {status}")

# Final drain
time.sleep(1)
leftover = s.read(s.in_waiting or 500)
if leftover:
    print(f"\nLeftover bytes: {' '.join(f'{x:02X}' for x in leftover)}")

print("\n--- Done. Port stays open until you press Ctrl+C ---")
print("(Keeping port open prevents ESP32 reset)")
try:
    while True:
        time.sleep(1)
        data = s.read(s.in_waiting or 1)
        if data:
            print(f"  Incoming: {' '.join(f'{x:02X}' for x in data)}")
except KeyboardInterrupt:
    print("\nClosing port.")
    s.close()
