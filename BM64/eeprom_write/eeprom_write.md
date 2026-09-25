# BM64 EEPROM Write — Procedure & Standalone ESP32 Writer Plan

## CONFIRMED 2026-09-25: Full EEPROM read/write protocol (HCI ACL Data)

**Breakthrough:** the EEPROM commands are NOT raw bytes on the wire — they
travel as **HCI ACL Data packets** to connection handle `0x0FFF`, wrapped
inside the standard HCI transport. The `0A`/`0B` bytes seen in DMS captures
are the ACL *payload*, not the wire-level framing. Confirmed end-to-end on
live hardware via the ESP-IDF console:

- **Address 0x0094 = 0x73** ✓ (codec function — headphone DAC)
- **Address 0x0348 = 0x0A** ✓ (single-end with earphone — the value we wrote)

### Required init sequence (must run once after entering IBDK mode)

```
1. 01 31 FC 0C 00 02 00 00 00 00 00 00 00 02 00 00   (vendor init 0xFC31)
2. 01 05 10 00                                         (Read_Buffer_Size)
3. 01 33 0C 07 01 00 02 03 00 04 00                    (Host_Buffer_Size)
4. 01 05 04 0D 00 00 00 00 00 00 00 00 00 00 00 00 00 (Create_Connection → handle 0xFFF)
5. 02 FF 0F 07 00 00 01 03 00 05 02 00                 (ACL setup: memory type)
6. 02 FF 0F 04 00 20 00 00 00                           (ACL setup: buffer config)
```

### EEPROM READ command (ACL Data to handle 0xFFF)

```
02 FF 0F 0E 00  10 01  0A 00 05 02  [ADDR LE32]  [LEN LE32]
│  └─handle──┘  │  │   └─EEPROM──┘
│    ACL hdr    │  │    read type
│   len=14      │  └─status
└─ACL indicator └─sub-cmd: read
```

Response: `04 13 05 ...` (NumCompleted) + `02 FF 0F [len] 00 10 01 [payload_len LE32] [count LE32] [DATA...]`

### EEPROM WRITE command

```
02 FF 0F 0F 00  11 01  0B 00 05 02  [ADDR LE32]  [LEN LE32]  [DATA...]
                │  │   └─EEPROM──┘
                │  │    write type
                │  └─status
                └─sub-cmd: write
```

### Firmware commands

- **`bm64_ibdk`** — automates pin toggle + full init sequence (all 6 steps)
- **`bm64_eeprom read <hex_addr> [len]`** — reads EEPROM, parses and prints data
- **`bm64_eeprom write <hex_addr> <hex_byte>`** — writes single byte, confirms OK

### Earlier DMS analysis (still valid, just reinterpreted)

The DMS captures (`...084134.dmslog8`, `...085143.dmslog8`) showed the
`0A 00 05 02` / `0B 00 05 02` EEPROM payload bytes inside `02 FF 0F`
ACL records preceded by `00 04 00 00 00` outgoing-data markers. The raw
`0A`/`0B` commands sent without ACL wrapping always returned `04 10 01 10`
(HCI Hardware Error). The `02 FF 0F` prefix was initially misread as an
IOCTL control code, but is actually the HCI ACL Data header for handle
0x0FFF.

---

## TOMORROW: quick reference (do this first)

**The one byte that fixes audio:** address `0x348` currently holds `0x09`
("capless with earphone") but needs to be `0x0A` ("single-end with
earphone") to agree with `0x1D7` (already correctly `0x94` = Single End) --
see "Found 2026-09-24 (evening)" section below for the full reasoning. This
is a real inconsistency in Microchip's own `BM64CLS1` reference table, not
something done wrong during the original flash.

1. isUpdate Tool -> memory type = **eeprom** -> connect
2. **Write** single byte: Address `0348`, Length `01`, Value `0A`
3. **Verify**: Read Address `0348`, Length `01` -> should now read `0A`
4. Also re-verify `0x1D7` still reads `94` (bit4=1, Single End) -- should be
   unchanged, just confirming nothing else drifted
5. Return to normal mode, re-pair, re-test audio (record + inspect for real
   varying content, not a flat/repeating tone -- see the analysis approach
   in "Verify audio output" below)

**If you also want to capture the read-EEPROM protocol** (still unconfirmed
-- see "Standalone ESP32 EEPROM Writer" section for everything ruled out
2026-09-24/25 night): while isUpdate is connected and in eeprom mode, do a
plain **Read** of some address (0x0348 or 0x1D7 work fine, or the OS's own
DMS capture tool, whichever you used before) *before* doing the Write above,
and save that capture. The `MCHP_DSPKv2.1.7_BM64CLS1.txt`-write capture
already analyzed does NOT contain a read -- confirmed by finding the tool's
own "read device name" query (opcode `0xFC20`) appearing exactly twice
(matching connect -> write -> reconnect, no read/verify step in between).
A capture of a genuine Read is the single most useful thing to bring back.

---

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

### Update 2026-09-25 (macOS session, continued): live probing + deeper log analysis

**Live probing on real hardware**, once BM64 was confirmed to genuinely
enter IBDK mode (see "GPIO fix" note below -- this took most of the session
to get reliable) via the ESP-IDF firmware's new `bm64_raw <hex bytes>
[listen_ms]` console command (`firmware/components/bringup/bringup.c` --
sends raw non-AA00-framed bytes straight to BM64's UART and prints whatever
comes back, no separate PC bridge tool needed):

- Confirmed `0xFC09` (memory read) works exactly as documented: address
  `0x0000` returns `0x55` (matches the original `probe_hci.py` finding
  exactly), address `0x01D7` returns `0x00` (unprogrammed flash at that
  offset -- NOT the real EEPROM value `0x94`, confirming this reads flash).
- **Ruled out**: a 4th inline parameter byte on the `0xFC09` read command
  (guessing it might select memory type) -- no effect, still reads flash.
- **Ruled out**: `0xFC0B` as a memory-type selector -- swept parameter
  values `0x00, 0x01, 0x02, 0x03, 0x10, 0xFF`, always gets a generic ack,
  never changes what `0xFC09` subsequently reads.
- **Ruled out**: a raw (non-HCI) `[CMD][00][ADDR_LO][ADDR_HI][00][00]`
  6-byte header read, CMD swept `0x00`-`0x0F` at address 0 -- most CMD
  values return nothing or a generic `04 10 01 10` error; none returned
  plausible EEPROM content.

**Deeper analysis of the existing `.dmslog8` capture** (see the "DMS Log
Reverse-Engineering Findings" section above for the original write-block
find) confirmed the file has only **92 total `[00 00 00 00][LEN][data]`
records** (29 small + 63 large) -- NOT "1954" as an earlier session's notes
said; that number was simply wrong. All 92 were printed and reviewed
directly (`analyze_pre_write.py`, kept in this folder). Two genuine,
non-coincidental commands were found by searching for the literal ASCII
string the device would report at connect time (`5506_DUAL_SPK`, the ROM
name from the `.txt` table's own header comment):

- **`0xFC20`** = read device/ROM name. Reply: `01 20 FC` + null-terminated
  ASCII string (`"5506_DUAL_SPK"` + `00`) + a few trailing bytes
  (`42 15 00 01 00 00`).
- **`0xFC31`** = some other status query (params `0C 00 02 00 00 00 00 00
  00 00 02`, meaning unconfirmed) -- appears right after `0xFC20` both times.

Both appear **exactly twice** in the file, matching connect -> write ->
disconnect -> reconnect -> disconnect (confirmed against memory of the
session: connect once, write the whole payload, disconnect/reconnect,
never ran a Read/verify query). This is why no read-EEPROM command could be
found in this file: **it was never captured, because it was never
performed** -- not a parsing failure. A systematic sweep of every distinct
`01 xx FC` opcode in the whole file turned up only these two plus two
likely-coincidental matches (`0xFC82`, `0xFCFF`) inside dense
audio-parameter table data with none of the clean framing (FILETIME
markers, isolated position) the two real ones had.

### GPIO fix that unblocked all of this (2026-09-25)
Bench-wired MFB/RST_N/P2_0 (GPIO18/19/20) intermittently failed to hold a
level for most of two evenings, looking exactly like a wiring fault. Root
cause: `bm64_pin`/`assert_bm64_control_pins()` used a bare
`gpio_set_direction()` + `gpio_set_level()` pair, which never touches
`pull_up_en` -- a leftover internal pull-up was fighting the output. Fixed
by using the full `gpio_config()` struct instead (explicit
`pull_up_en`/`pull_down_en` in the same atomic call as the mode), matching
this project's already-correct `ag1170.c` SLIC-pin pattern. Full writeup:
`archive/BM64_MFB_GPIO_DEBUG.md` in the main repo. This is why live IBDK
probing wasn't reliable until partway through tonight's session -- keep it
in mind if GPIO18-20 seem flaky again, don't assume it's the wiring first.

### NEXT ACTION, next isUpdate session
Do a plain **Read** (not Write) of any EEPROM address in isUpdate Tool
(memory type = eeprom), with the same DMS-log-style capture running, then
re-run `analyze_pre_write.py`'s approach (or hand-search for `01 xx FC`
opcodes as done above) against that new capture -- a real Read is the one
thing this file provably never contained. Comparing that capture's opcodes
against the confirmed `0xFC20`/`0xFC31` pair (and definitely-not `0xFC09`,
since that's a flash-only debug read unrelated to isUpdate's own protocol)
should surface the real read-EEPROM command directly.

### Files needed on macOS (nothing Windows-specific required for read tests)
- `analyze_pre_write.py` (new 2026-09-25, this folder -- prints all 92
  records with the write block marked; the opcode-sweep one-liners used to
  find `0xFC20`/`0xFC31` above aren't saved as a script yet, easy to redo:
  search the raw file bytes for `b'\x01' + bytes([opcode_lsb, 0xFC])` at
  every position)
- `eeprom_write.md`, `BM64_Analog_Audio_Setup.md` (this doc + the main doc)
- The physical ESP32-C6 + BM64 hardware, wired per `BM64_Analog_Audio_Setup.md`
  Step 1 (GPIO16/17/18/19/20) -- `firmware/bm64_raw` console command
  (main repo, `idf.py -B build -p <port> flash` then use the bring-up
  console) can now do all the live probing directly, no separate Arduino
  bridge or PC tool needed for probing (still need isUpdate for the actual
  EEPROM byte 0x348 fix and any fresh DMS capture).

**Not needed on macOS:** the `.dmslog8` capture file, `isupdate.exe`,
`EEPROM_Tool.exe` (Windows-only tools) — already fully mined for the finding
above; `find_all_eeprom_chunks.py`/`test_eeprom_raw_protocol.py` (both
superseded by live probing via `bm64_raw`, don't re-run).

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
