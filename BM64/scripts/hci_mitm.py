"""
HCI Man-in-the-Middle logger.

Setup:
  1. Install com0com, create virtual pair COM20 <-> COM21
  2. Run this script: python hci_mitm.py
  3. Open isUpdate, select COM21 (virtual port)
  4. Perform EEPROM read/write in isUpdate
  5. Stop script with Ctrl+C — log saved to hci_log.bin and hci_log.txt

Wiring:
  isUpdate <--COM21--> [com0com pair] <--COM20--> [this script] <--COM9--> ESP32 bridge <--UART--> BM64
"""
import serial
import threading
import time
import sys
import struct

# === CONFIG ===
REAL_PORT = 'COM9'       # Real ESP32 bridge
VIRTUAL_PORT = 'COM20'   # Our end of the com0com pair (isUpdate connects to COM21)
BAUD = 115200
LOG_FILE = 'hci_log.txt'
RAW_LOG = 'hci_log.bin'

# === Globals ===
lock = threading.Lock()
running = True
log_f = None
raw_f = None
start_time = None

# HCI opcode names we know
OPCODE_NAMES = {
    0x0C03: "HCI_Reset",
    0xFC09: "VND_MemRead",
    0xFC0A: "VND_MemWrite?",
    0xFC0B: "VND_RegAccess",
    0xFC20: "VND_DeviceID",
    0xFC29: "VND_MemRead2",
    0xFC31: "VND_Config",
    0xFC62: "VND_Setup",
    0xFCA4: "VND_Status",
}

def ts():
    return time.time() - start_time

def log(direction, data):
    """Log data with timestamp and direction."""
    if not data:
        return
    t = ts()
    hexstr = ' '.join(f'{b:02X}' for b in data)
    
    with lock:
        # Raw binary log: [direction_byte] [4-byte timestamp_ms] [2-byte length] [data]
        raw_f.write(bytes([0 if direction == 'TX' else 1]))
        raw_f.write(struct.pack('<I', int(t * 1000)))
        raw_f.write(struct.pack('<H', len(data)))
        raw_f.write(data)
        raw_f.flush()
        
        # Human-readable log
        line = f"[{t:10.3f}] {direction:2s} ({len(data):4d}): {hexstr}\n"
        log_f.write(line)
        log_f.flush()
        
        # Console output with HCI parsing
        parsed = parse_hci(direction, data)
        print(f"[{t:8.3f}] {direction} ({len(data):3d}B): {parsed}")

def parse_hci(direction, data):
    """Parse HCI packet for display."""
    if not data:
        return ""
    
    ptype = data[0]
    
    if ptype == 0x01 and len(data) >= 4:
        # HCI Command
        opcode = data[1] | (data[2] << 8)
        plen = data[3]
        params = data[4:4+plen]
        name = OPCODE_NAMES.get(opcode, f"FC{opcode & 0xFF:02X}")
        params_hex = ' '.join(f'{b:02X}' for b in params[:24])
        if len(params) > 24:
            params_hex += "..."
        return f"CMD {name} (0x{opcode:04X}) params=[{params_hex}]"
    
    elif ptype == 0x04 and len(data) >= 3:
        # HCI Event
        evt_code = data[1]
        plen = data[2]
        params = data[3:3+plen]
        
        if evt_code == 0x0E and len(params) >= 3:
            # Command Complete
            num = params[0]
            opcode = params[1] | (params[2] << 8)
            name = OPCODE_NAMES.get(opcode, f"FC{opcode & 0xFF:02X}")
            status = params[3] if len(params) > 3 else -1
            rest = params[4:]
            rest_hex = ' '.join(f'{b:02X}' for b in rest[:32])
            if len(rest) > 32:
                rest_hex += f"... (+{len(rest)-32}B)"
            return f"EVT CmdComplete {name} status={status} data=[{rest_hex}]"
        
        elif evt_code == 0x0F:
            return f"EVT CmdStatus"
        elif evt_code == 0x10:
            return f"EVT HardwareError"
        else:
            hex_data = ' '.join(f'{b:02X}' for b in params[:20])
            return f"EVT code=0x{evt_code:02X} [{hex_data}]"
    
    else:
        hexstr = ' '.join(f'{b:02X}' for b in data[:32])
        if len(data) > 32:
            hexstr += f"... (+{len(data)-32}B)"
        return hexstr

def relay(src, dst, direction, src_name):
    """Relay thread: read from src, write to dst, log."""
    global running
    try:
        while running:
            if src.in_waiting:
                data = src.read(src.in_waiting)
                if data:
                    dst.write(data)
                    dst.flush()
                    log(direction, data)
            else:
                time.sleep(0.001)  # 1ms polling
    except Exception as e:
        print(f"\n[ERROR] {src_name} relay: {e}")
        running = False

def main():
    global log_f, raw_f, start_time, running
    
    print("=== HCI Man-in-the-Middle Logger ===")
    print(f"Real port:    {REAL_PORT} (ESP32 bridge)")
    print(f"Virtual port: {VIRTUAL_PORT} (our end of com0com pair)")
    print(f"isUpdate should connect to the OTHER end of the pair")
    print()
    
    try:
        real = serial.Serial(REAL_PORT, BAUD, timeout=0.01)
        print(f"Opened {REAL_PORT}")
    except Exception as e:
        print(f"FAILED to open {REAL_PORT}: {e}")
        return
    
    try:
        virt = serial.Serial(VIRTUAL_PORT, BAUD, timeout=0.01)
        print(f"Opened {VIRTUAL_PORT}")
    except Exception as e:
        print(f"FAILED to open {VIRTUAL_PORT}: {e}")
        print("Make sure com0com is installed and the virtual pair is created")
        real.close()
        return
    
    log_f = open(LOG_FILE, 'w')
    raw_f = open(RAW_LOG, 'wb')
    start_time = time.time()
    
    print(f"\nLogging to {LOG_FILE} and {RAW_LOG}")
    print("Waiting for traffic... (Ctrl+C to stop)\n")
    print("-" * 80)
    
    # Start relay threads
    # TX = isUpdate -> BM64 (virtual -> real)
    # RX = BM64 -> isUpdate (real -> virtual)
    t_tx = threading.Thread(target=relay, args=(virt, real, 'TX', 'virtual'), daemon=True)
    t_rx = threading.Thread(target=relay, args=(real, virt, 'RX', 'real'), daemon=True)
    t_tx.start()
    t_rx.start()
    
    try:
        while running:
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\n\n=== Stopping ===")
    finally:
        running = False
        time.sleep(0.2)
        real.close()
        virt.close()
        log_f.close()
        raw_f.close()
        print(f"Logs saved to {LOG_FILE} and {RAW_LOG}")

if __name__ == '__main__':
    main()
