"""
BM64 Bridge Sniffer — tries various identification sequences in both
normal and IBDK modes to figure out what EEPROM_Tool expects.

Run with: python -u sniff_bridge.py

Make sure Serial Monitor is CLOSED before running this.
"""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

def open_port():
    try:
        s = serial.Serial(PORT, BAUD, timeout=2)
        time.sleep(0.5)
        s.reset_input_buffer()
        return s
    except serial.SerialException as e:
        print(f"FAILED to open {PORT}: {e}")
        print("Close Arduino Serial Monitor first!")
        sys.exit(1)

def send_and_show(s, label, data):
    s.reset_input_buffer()
    s.write(data)
    time.sleep(1.0)
    resp = s.read(s.in_waiting or 200)
    sent_hex = ' '.join(f'{b:02X}' for b in data)
    if resp:
        resp_hex = ' '.join(f'{b:02X}' for b in resp)
        print(f"  {label:40s} SENT: {sent_hex}")
        print(f"  {'':40s} RECV: {resp_hex}")
        # Also show as ASCII where printable
        ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in resp)
        print(f"  {'':40s} ASCII: {ascii_str}")
    else:
        print(f"  {label:40s} SENT: {sent_hex}  -> NO RESPONSE")
    print()

def enter_programming_mode(s):
    """Send 'F' to Arduino sketch to enter IBDK mode"""
    s.reset_input_buffer()
    time.sleep(0.1)
    s.write(b'F')
    time.sleep(2)
    resp = s.read(s.in_waiting or 500)
    print(f"  Programming mode response: {resp.decode(errors='replace')}")

def enter_normal_mode(s):
    """Send 'R' to Arduino sketch to reset to normal mode"""
    s.reset_input_buffer()
    time.sleep(0.1)
    s.write(b'R')
    time.sleep(2)
    resp = s.read(s.in_waiting or 500)
    print(f"  Normal mode response: {resp.decode(errors='replace')}")

# Normal UART protocol commands (AA 00 header)
UART_COMMANDS = [
    ("GET_UART_VERSION",     bytes([0xAA, 0x00, 0x02, 0x08, 0x00, 0xF6])),
    ("READ_FEATURES",        bytes([0xAA, 0x00, 0x02, 0x39, 0x00, 0xC5])),
    ("READ_DEVICE_NAME",     bytes([0xAA, 0x00, 0x02, 0x10, 0x00, 0xEE])),
    ("READ_BT_ADDRESS",      bytes([0xAA, 0x00, 0x02, 0x0F, 0x00, 0xEF])),
    ("READ_LINK_STATUS",     bytes([0xAA, 0x00, 0x02, 0x0D, 0x00, 0xF1])),
]

# Unknown/exploratory sequences for IBDK mode
IBDK_PROBES = [
    ("Single 0x01",          bytes([0x01])),
    ("Single 0x05",          bytes([0x05])),
    ("ISSC sync 0x01 0x00",  bytes([0x01, 0x00])),
    ("3x 0x55 (sync?)",      bytes([0x55, 0x55, 0x55])),
    ("AT command",           b'AT\r\n'),
    ("Query ? ",             b'?\r\n'),
    ("HCI reset",            bytes([0x01, 0x03, 0x0C, 0x00])),
    ("ISSC read ID",         bytes([0x01, 0x00, 0x00, 0x00])),
    ("Raw 0xAA",             bytes([0xAA])),
    ("EEPROM read 0x0000",   bytes([0x02, 0x00, 0x00])),
]

print(f"=" * 70)
print(f"BM64 Bridge Sniffer on {PORT}")
print(f"=" * 70)

s = open_port()

# --- Test 1: Normal mode ---
print(f"\n{'=' * 70}")
print("PHASE 1: BM64 in NORMAL mode — standard UART commands")
print(f"{'=' * 70}\n")

enter_normal_mode(s)
time.sleep(1)

for label, cmd in UART_COMMANDS:
    send_and_show(s, label, cmd)

# --- Test 2: Programming/IBDK mode ---
print(f"\n{'=' * 70}")
print("PHASE 2: BM64 in IBDK/PROGRAMMING mode — probing protocols")
print(f"{'=' * 70}\n")

enter_programming_mode(s)
time.sleep(1)

# First try normal UART commands (shouldn't work in IBDK, but let's see)
for label, cmd in UART_COMMANDS[:2]:
    send_and_show(s, f"[IBDK] {label}", cmd)

# Now try IBDK-specific probes
for label, probe in IBDK_PROBES:
    send_and_show(s, f"[IBDK] {label}", probe)

# --- Reset back to normal ---
print(f"\n{'=' * 70}")
print("Resetting BM64 back to normal mode...")
print(f"{'=' * 70}\n")
enter_normal_mode(s)

# Quick verify normal mode still works
send_and_show(s, "Verify: GET_UART_VERSION", UART_COMMANDS[0][1])

print("Done. Closing port.")
s.close()
