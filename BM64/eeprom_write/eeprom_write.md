# BM64 EEPROM Write — Procedure & Standalone ESP32 Writer Plan

## What Was Done (2026-09-24)

### Problem
BM64 shipped with speaker output config (`IS2064S_012_SPK02_V2.1_BETA`).
Need headphone DAC output on A0HPL/A0HPR + mic input on MIC_P1.

### Solution
Flashed EEPROM with `MCHP_DSPKv2.1.7_BM64CLS1.txt` (headphone DAC config)
using `isUpdate Tool v3.14` via an ESP32-C6 Arduino bridge sketch.

### Verified EEPROM bytes (headphone DAC)
| Address | Value | Meaning |
|---------|-------|---------|
| 0x0094  | 0x73  | Codec function — headphone DAC output |
| 0x0171  | 0x46  | Audio path config |
| 0x01FC  | 0x4A  | Audio routing |

---

## Found 2026-09-24 (evening): Capless vs Single-End conflict -- needs a fix

After flashing the above and getting BM64 paired/connected over Bluetooth,
a recording off the codec's line input showed a very weak (~0.8% full
scale), non-musical signal dominated by clean tones at 800Hz/1600Hz with a
flat, non-varying envelope -- not real audio, more like a switching/DAC
artifact. Root cause found by decoding the actual `MCHP_DSPKv2.1.5_BM64CLS1.txt`
table (the flashed file, `DSPK v2.1.5 Package/Software/EEPROM Table/`; the
v2.1.7 version at work should be identical or very close) against the
official parameter table (`Tools/UI Tool/BT5506_012UI_Parameter_Table_R0.xls`):

| Address | Field | Value in the flashed file | Meaning |
|---------|-------|---------------------------|---------|
| **0x1D7** | `CODEC_STEREO_DAC_path` bit4 | `0x94` -> bit4=**1** | **Single End** |
| **0x348** | `codec_parameter[0]` bits2:0 | `0x09` -> bits2:0=**001** | **capless with earphone** |

These two fields are explicitly cross-referenced as "related" to each other
in Microchip's own parameter table (each row's docs point at the other,
"Refer to 0x1D7/0x1D8 bit4" / "Refer to 0x348 bit2~0") -- **and they
disagree with each other in the file that was actually flashed.** This is a
real inconsistency in Microchip's reference table, not something done
wrong during flashing. A DAC output stage straddling two conflicting mode
selections is a very plausible explanation for a weak, artifact-dominated
output instead of clean audio.

Our physical wiring uses a coupling cap on A0HPL with AOHPM left floating --
that's the correct topology for **Single-End** mode (matches what 0x1D7
already selects), so the fix is to make 0x348 agree with 0x1D7, not to
rewire anything.

### The fix (do this first, before anything else, next isUpdate session)

Change **address `0x348`** from `0x09` to **`0x0A`** (only bits2:0 change,
`001`->`010`: "capless with earphone" -> "single-end with earphone"). Per
the parameter table, `codec_parameter[0]` has no other named bits in use
(neighboring bytes 0x349/0x34B are separate, unrelated parameters), so this
is a clean, isolated one-byte change -- no risk of disturbing anything else.

Procedure (same tool/steps already proven working tonight):
1. isUpdate Tool -> memory type = **eeprom** -> connect as before
2. **Write** single byte: Address `0348`, Length `01`, Value `0A`
3. **Verify**: Read Address `0348`, Length `01` -> should now read `0A`
4. Also re-verify `0x1D7` still reads `94` (bit4=1, Single End) -- should be
   unchanged, just confirming nothing else drifted
5. Return to normal mode, re-pair, re-test audio (record + inspect for real
   varying content, not a flat/repeating tone)

Checked `0x1D8` (`CODEC_MONO_DAC_path` bit4, the mono-path counterpart to
0x1D7's stereo path, same "0:Capless / 1:Single End" encoding) too: flashed
value is `0x93` -> bit4=**1** -> Single End, consistent with 0x1D7. So only
`0x348` is out of alignment -- the single-byte fix above should be
sufficient.

---

## Step-by-Step Flash Procedure

### Prerequisites
- ESP32-C6 wired to BM64: TX=GPIO16, RX=GPIO17, MFB=GPIO18, RST_N=GPIO19, P2_0=GPIO20
- Arduino IDE installed with ESP32-C6 board support
- `bm64_bridge_arduino/bm64_bridge_arduino.ino` sketch (auto-bridge, BOOT_MODE 0)

### 1. Upload the bridge sketch
- Open `bm64_bridge_arduino.ino` in Arduino IDE
- Set `#define BOOT_MODE 0` (programming/flash mode)
- Board: "ESP32-C6 Dev Module", USB CDC On Boot: "Enabled", Baud: 115200
- Upload, then **fully quit Arduino IDE** (not just close Serial Monitor)

### 2. Open isUpdate Tool
- Run `isUpdate Tool v3.14` (from `DSPK+v2.1.7+Package/Tools/FlashUpdate Tool/`)
- Port: COM9 (or whatever your ESP32-C6 enumerates as), baudrate: 115200
- Click **Connect** — should show "Port connect -> COMx"
- Device field should show `5506_SPK02` (or similar)

### 3. Write EEPROM table
- Change **Memory Type → memory** dropdown to **eeprom**
- In the bottom **Flash/EEPRom/MCU/AHB Access** section:
  - Click **Browse** (next to "Write Table")
  - Select: `DSPK+v2.1.7+Package/Software/EEPROM Table/MCHP_DSPKv2.1.7_BM64CLS1.txt`
  - Click **Write Table**
- Wait for completion

### 4. Verify
- With memory type still set to **eeprom**, use the Read section:
  - Address: `0094`, Length: `01` → should read `73`
  - Address: `0171`, Length: `01` → should read `46`
  - Address: `01FC`, Length: `01` → should read `4A`

### 5. Return to normal mode
- Disconnect isUpdate Tool
- Open Arduino IDE, change `#define BOOT_MODE 1`, upload
- BM64 boots in normal application mode with headphone DAC config

---

## Key Gotchas Discovered

1. **Arduino IDE holds COM port** even when Serial Monitor is closed. Must
   fully **quit** Arduino IDE before flash tools can connect.

2. **ESP32-C6 DTR reset**: opening/closing the COM port resets the ESP32.
   The sketch must auto-enter programming mode in `setup()` so it survives
   the reset when tools open the port. Interactive F/R/T commands don't work
   because closing Serial Monitor resets the ESP32 and loses the mode.

3. **ESP-IDF `bm64_bridge` firmware is broken**: the console's VFS layer
   keeps owning USB-CDC input after the `bridge` command, so the raw
   forwarding loop never receives bytes from the host. Use Arduino sketch.

4. **BM64 in IBDK mode speaks HCI** (Host Controller Interface), not the
   normal AA 00 UART protocol. Flash tools use HCI vendor-specific commands.

5. **EEPROM_Tool.exe** could identify the module but failed to write
   ("cannot disconnect the device" error). **isUpdate Tool** succeeded.

6. **The .txt EEPROM files** use `**` for "preserve current value" — BT
   address and other device-specific bytes are not overwritten.

---

## DMS Log Reverse-Engineering Findings (2026-09-24, continued)

### Background
Captured a USB DMS log (`USB Serial Device (COM9)$20260924-160549.dmslog8`,
269KB) while isUpdate Tool wrote the EEPROM table, to find the actual raw
bytes sent to the BM64 (since HCI opcode brute-forcing in `probe_hci.py`
never found the EEPROM write/select command).

### DMS log binary format (reverse-engineered)
The `.dmslog8` file wraps every USB `WriteFile`/`ReadFile` call in a
Windows-driver-style record:
```
[00 00 00 00][LEN: uint32 LE][... LEN bytes of raw serial data ...]
```
Records are interspersed with unrelated IOCTL/status-polling noise (a fixed
repeating pattern: `00 02 FF 0F 8E 00 11 01 8A 00 05 02 ...` — this is NOT
serial traffic, just internal DeviceIoControl chatter) and FILETIME-stamped
records (8-byte Windows FILETIME, recognizable by trailing `79 4C DD 01` /
`6A 79 4C DD 01` etc. — the DD 01 high bytes are consistent because the
capture was all done within the same few minutes).

### Confirmed real EEPROM write packet
Found by extracting all `[00 00 00 00][LEN][data]` records (1954 total) and
scoring each against the known-good EEPROM table
(`MCHP_DSPKv2.1.7_BM64CLS1.txt`), looking for long high-confidence byte
matches. Exactly **one** genuine write showed up (appearing twice — likely a
write + read-back verification):

```
file offset 0x02313B and 0x0231FF (128 bytes each)
04 00 00 00 00 00 01 04 04 24 00 0A 16 00 B8 06 00 00 31 11 10 18 09 2A 09 00
03 0A 00 9B 00 AE 31 03 05 BF FE FF FE 98 3D 19 86 01 0C 72 10 00 00 00 10 3C
00 00 0A CE 01 A4 03 00 00 52 5A CE E2 00 00 00 00 10 11 30 30 30 30 00 ... (zero-padded to 128 bytes)
```
— **121/121 bytes match EEPROM addresses 0x0006–0x007B exactly (100%)**.

Structure: **`[6-byte header: 04 00 00 00 00 00][122 bytes of EEPROM payload]`**
sent as a single 128-byte low-level write (this is NOT standard HCI framing
— it doesn't start with `01/FC` — confirming isUpdate uses a separate raw
protocol for EEPROM specifically, distinct from the `AA 00`/HCI command
sets already documented).

Working theory for the 6-byte header: `[CMD=0x04][RESERVED=0x00][ADDR_LO=0x00][ADDR_HI=0x00][RESERVED=0x00][RESERVED=0x00]`
— i.e. CMD 4 = "write 128-byte EEPROM block", address = 0x0000 (block 0).
The header physically overlaps where EEPROM addresses 0x0000-0x0005 (the
preserved BT MAC, marked `**` in the .txt table) would be, so the real
payload naturally starts at 0x0006. **This is currently unverified —
only one address (block 0) was captured, so the ADDR field's encoding
can't be confirmed without a second sample at a different address.**

### Why we didn't find more blocks / don't need to
- The EEPROM table is ~7955 bytes (`0x1F13`), so a full table write would
  need ~62 such 128-byte blocks — but the log only shows this ONE.
- Checked large `WriteFile`/IOCTL buffer records (500–20000 bytes, 63 found)
  for embedded EEPROM content — best matches were only 14–46%, all
  attributable to coincidental zero-byte matches against empty EEPROM
  regions, not genuine data.
- **Conclusion:** most of the table is default/zero, so any block covering
  an all-zero region is indistinguishable from generic IOCTL noise — those
  blocks likely *were* sent, we just can't isolate them from noise. The one
  block we found stands out only because it contains real non-zero config
  bytes (addr 0x0006 region). This is sufficient to attempt the experiment —
  full table coverage isn't needed to validate the wire format.

### Scripts used (kept in repo for reference)
- `extract_dmslog_data2.py` — fast, record-based extraction (~1954 records,
  runs in ~1-2 sec). **Preferred approach** — use this, not a byte-by-byte
  scan (see below).
- `find_all_eeprom_chunks.py` — brute-force per-byte-position scan across
  the whole 269KB file. Much slower and prone to false positives from
  generic ascending-byte tables in the EEPROM (e.g. addr 0x0180 region
  looked like a match at first but turned out to be an unrelated Bluetooth
  AFH channel-map constant coincidentally identical elsewhere in memory).
  Not recommended — the record-based method is both faster and more
  reliable since it aligns to actual WriteFile/ReadFile call boundaries.
- `dump_write_cmd.py <hex_offset>` — dumps raw bytes around any file offset
  for manual inspection (usage: `python dump_write_cmd.py 0x02313B`).

---

## Standalone ESP32 EEPROM Writer (TODO)

### Goal
Have the ESP32-C6 program BM64's EEPROM directly over UART without any PC
tools. On boot (or on command), the ESP32 puts BM64 into IBDK mode, sends
HCI vendor-specific commands to write the EEPROM, then resets BM64 to normal.

### What we know (from probe_hci.py, 2026-09-24)
- BM64 in IBDK mode (P2_0 LOW during reset) speaks HCI at 115200 baud
- HCI_Reset (`01 03 0C 00`) returns Command Complete (`04 0E 04 01 03 0C 00`)
- **`0xFC09`** = memory read command. Params: `[addr_lo, addr_hi, length]`.
  Returns `04 0E [len] 01 09 FC 00 [addr_lo] [addr_hi] [length] [data...]`
  By default reads flash (got 0x55 from addr 0x0000). Need memory-type
  selection to read EEPROM.
- **`0xFC0B`** = likely memory-type select or write-control command. Always
  returns short ack `04 0E 06 01 0B FC 00 ...`. Needs further testing to
  determine if it selects eeprom/flash/emcu.
- isUpdate uses memory dropdown (flash/eeprom/emcu) — probably sends FC0B
  or similar to select the target before FC09 read / write ops.
- **Next step**: sniff isupdate traffic (using a com0com relay or logging
  bridge) to capture the exact command sequence for eeprom read/write.
- The EEPROM data to write comes from `MCHP_DSPKv2.1.7_BM64CLS1.txt`
  (address, length, value triples; `**` entries are skipped)

### Steps to implement
1. **Discover HCI commands**: Run `probe_hci.py` to find the vendor-specific
   opcodes for EEPROM read and write
2. **Parse .txt EEPROM table**: Convert the `address,length,value` lines
   (skipping `**` entries) into a C array for the ESP32 firmware
3. **Implement HCI writer on ESP32**:
   - Put BM64 into IBDK mode (P2_0 LOW + reset pulse)
   - Send HCI_Reset, wait for Command Complete
   - For each EEPROM entry: send HCI write command, wait for ack
   - Reset BM64 to normal mode (P2_0 HIGH + reset pulse)
4. **Verify**: Read back key bytes (0x0094, 0x0171, 0x01FC) and compare

### NEXT ACTION (continue here — macOS session)
Run **`test_eeprom_raw_protocol.py`** (cross-platform, pyserial-based) — it
does NOT use HCI opcode framing at all. Based on the DMS-log finding above,
it sends raw `[CMD][RESERVED][ADDR_LO][ADDR_HI][RESERVED][RESERVED]` guesses
(READ-only, non-destructive) to find the matching read command and confirm
whether byte 1 is really an address field:
```
python test_eeprom_raw_protocol.py [PORT]
```
- No PORT arg needed — it lists available ports and auto-selects if only one.
- On macOS the ESP32-C6 will enumerate as something like
  `/dev/tty.usbmodemXXXX` — Arduino IDE must be fully quit first, same as
  on Windows.
- Looks for byte `0x73` (known EEPROM[0x0094] value) appearing in any
  response, across all 3 test batteries (header CMD sweep with addr=0,
  CMD=0x04 variants with addr=0x0094, and full CMD sweep with addr=0x0094).
- If nothing hits, the next step is a live sniff: relay isUpdate through a
  logging proxy (e.g. `com0com` + a Python passthrough on Windows) while
  doing a **Read** (not Write) in isUpdate Tool's eeprom mode, then re-run
  `extract_dmslog_data2.py` against that capture instead — reads are
  smaller/simpler than the write we already decoded and should surface the
  read opcode quickly using the same method.

### Files needed on macOS (nothing Windows-specific required for read tests)
- `test_eeprom_raw_protocol.py` (new, cross-platform)
- `probe_hci.py` (cross-platform, but PORT hardcoded to `COM9` — change to
  your macOS device path or pass as arg if you adapt it similarly)
- `eeprom_write.md`, `BM64_Analog_Audio_Setup.md` (this doc + the main doc)
- `bm64_bridge_arduino/bm64_bridge_arduino.ino` (flash to ESP32-C6 first,
  BOOT_MODE 0, via Arduino IDE on macOS)
- `DSPK+v2.1.7+Package/Software/EEPROM Table/MCHP_DSPKv2.1.7_BM64CLS1.txt`
  (ground-truth table, referenced by `KNOWN_EEPROM_BYTES` in the test script)
- The physical ESP32-C6 + BM64 hardware, wired per `BM64_Analog_Audio_Setup.md`
  Step 1 (GPIO16/17/18/19/20)

**Not needed on macOS:** the `.dmslog8` capture file, `isupdate.exe`,
`EEPROM_Tool.exe` (Windows-only tools) — already fully mined for the finding
above; `find_all_eeprom_chunks.py` (superseded, don't re-run — see notes above).

### Architecture
```
ESP32-C6 firmware:
  setup():
    if (button pressed OR first boot):
      enter_ibdk_mode()
      hci_reset()
      write_eeprom_table(bm64_cls1_eeprom, sizeof(bm64_cls1_eeprom))
      verify_eeprom()
      reset_to_normal()
    else:
      normal_bluetooth_audio_operation()
```
