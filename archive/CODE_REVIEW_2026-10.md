# Code review, October 2026

A systematic pass over the firmware, area by area, looking for bugs (memory
safety, races, input handling, security) and things worth improving. Tools:
cppcheck 2.21 over the real build configuration (`compile_commands.json`),
plus reading each area by hand. Every fix below was built, flashed to an
SMD4 (8 MB) and tested on it where the behaviour can be exercised without a
person at the phone.

## Fixed

| Area | Problem | Effect before the fix | Commit |
|------|---------|-----------------------|--------|
| Web forms | `form_field()` cut the %XX-encoded value to the buffer size *before* decoding | A WiFi or SIP password with several symbols was silently saved short (each symbol is 3 encoded bytes) | `f5327ef` |
| Web forms | WiFi/VoIP form body limits (384/512 B) smaller than a full-length, fully encoded form | Long passwords with symbols rejected with "bad form body" | `f5327ef` |
| Storage | Directory listing stopped at 64 files in readdir order, then sorted | With >64 files (an SD card of recordings) the Recordings/Storage pages showed an arbitrary 64, not the newest | `f5327ef` |
| Storage | File names from URLs/uploads went straight into paths | Path tricks (`/`, `..`) reached the filesystem layer; now refused centrally | `f5327ef` |
| Web UI | Bulk delete truncated names before URL-decoding them | Files whose encoded name was long couldn't be bulk-deleted | `f5327ef` |
| Web UI | WAV duration divided by `bits_per_sample / 8` | Divide by zero on a <8-bit WAV | `f5327ef` |
| WiFi | `wifi_config_t` unions zeroed with `= {0}` | Only the first union member is guaranteed zero; now `memset` | `f5327ef` |
| Player | A stopped clip's task called `audio_tx_silence()` after the FSM had already given the line new audio | **One-way audio when picking up during the answering machine's greeting** (local and VoIP barge-in); could also cut dial tone after a very quick re-pickup | `c6abae9` |
| SIP | INVITE/BYE/CANCEL accepted from any address | Any LAN host -- or an internet SIP scanner, once a board registers through a router to an internet server -- could ring the phone or end calls. Now only the configured server | `c6abae9` |
| SIP | `get_header()` left its output untouched when a header was missing | A packet without To/From/CSeq made the parser read uninitialized stack | `c6abae9` |
| Web UI | Diagnostics log not HTML-escaped, though it contains the first line of every SIP packet received and callers' names | **Script injection**: anything on the LAN could send a SIP packet that runs script in the owner's browser when they open Diagnostics | `c6abae9` |
| Web UI | Dial-guide descriptions (Status page), file names (Storage, Recordings, Dial Codes) and SSIDs not escaped; delete confirmation spliced names into JavaScript | Script injection via an uploaded file name or a clip description; legal names like `Tom & Jerry` broke pages | `c6abae9` |
| Web UI | `/media/<name>` never decoded `%20` etc. | **Files with spaces or symbols in their names couldn't be played, downloaded or deleted**; uploads by URL saved literal `%20` | `c6abae9` |
| Web UI | No protection against cross-site requests | **Any web page the owner visited could POST to the board**: change settings, delete recordings, upload firmware. Every POST now checks `Origin` | `9ac1d3a` |
| Settings | Dial codes and prompts are saved as raw struct arrays | A firmware that changes the entry layout (the planned dial-plan redesign will) would read every saved code back scrambled; such a blob is now ignored and defaults re-seeded | `603b6cc` |
| SLIC | Ring stop left F/R wherever the 20 Hz toggling stopped | About half of all answered calls ran with reversed loop polarity | `102d817` |
| Provisioner | Per-address rate-limit table never pruned | Unbounded growth on an internet-facing server | `102d817` |
| Dial codes | Upload wrote the file *before* validating the code, under the upload's own name | **Adding a clip named `operator.wav` (or a second `song.wav`) overwrote the operator message or the other clip**, even when the code was then rejected | `1b844ee` |
| VoIP script | The downloadable Asterisk script (run with sudo on the PBX) put the board name and SIP number where bash expands them | **A board name like `$(...)` -- settable by anyone on the LAN -- ran as root on the PBX**; a password with `$` was written wrong | `8c9b38f` |
| gen_prompt.py | Prompt text (which `--sync` fetches from a board) passed to `say`/`espeak-ng` as an argument | Text starting with `-` was parsed as an option, e.g. `say -o <path>` overwriting a file on your Mac | `98ed6f0` |
| SLIC | Ring-trip only stopped ringing on the on-hook to off-hook edge | **The self-test's ringer check or the `ring` command on a line whose handset was already up put ring voltage into the earpiece** | `9df5d61` |
| Diagnostics | No record of why the board restarted | A crash, watchdog or brownout looked like an ordinary reboot; now shown in Recent activity and `/api/status` | `d2b00ea` |

## Checked and fine

- **Audio engine (`audio.c`)**: buffer sizing, lock discipline, the DMA ring
  trade-off (documented). Taps are called without the lock, but every task
  that adds or removes one is lower priority than the audio task on a
  single-core chip, so it can't be interrupted mid-call by a remover.
- **Voice effects**: every delay line and comb buffer sized for its maximum
  delay at 8 and 48 kHz; no intermediate overflow.
- **DTMF detector**: Goertzel state and magnitudes stay well inside 32/64
  bits at maximum AGC gain.
- **Recorder**: sound. (Its silence threshold is applied after the AGC on
  purpose -- raw speech is ~-46 dBFS, below the threshold.)
- **Dial plan**: collector bounds, and clip-code collision checks in both
  directions against built-ins, other clips, `555` and the Bluetooth prefix.
- **Telephony FSM**: 32-event queue drained every 10 ms; stale clip/record
  completions are ignored once the state has moved on.
- **RTP**: packet parsing bounded; start/stop waits for both tasks to exit.
- **Bluetooth (BM64) UART**: frame reader bounded and checksummed; every
  event field length-checked; caller ID copy clamped.

- **Multipart parser, OTA upload, log buffer, activity log, idle power, task
  priorities, form-number validation, start-up order**: sound.

## Please test in the morning

- **Answering-machine barge-in** (the one-way audio fix): let a call go to
  the answering machine, pick up during the greeting, and check both sides
  hear each other.
- **Ring safety**: lift a handset, then run the self-test's ringer check on
  that line -- it should not ring.
- **Incoming VoIP call** (the SIP source check): call 1005 from one of
  101-104 and answer. A spoofed INVITE from another host was confirmed
  ignored; a real one from the server couldn't be tested without ringing
  the bell at night.

## Not changed -- worth a decision

- **Same-digit DTMF lockout (~205 ms)** may drop a repeated digit when an
  old push-button phone's *memory redial* sends digits at its fastest
  (70-100 ms tone + 70-100 ms gap): `5551005` could become `551005`. Test a
  stored number with doubled digits; if digits go missing, try 5 blocks
  (~128 ms) -- the 51 ms silence check already covers short contact chatter.
- **DNS rebinding** isn't stopped by the Origin check; it needs a malicious
  DNS server too. A login, or a per-session token in forms, would close it.
- **SD card pulled mid-use**: `sdcard_deinit()` frees the card while a
  player, recorder or download may be mid-read on another task, so pulling
  the card during playback can crash rather than just fail the read. Fixing
  it properly needs open-file accounting in the storage layer.
- **Firmware upload doesn't check the board type**: an SMD2 image uploaded
  to an SMD4 boots with the wrong pin map, and since it starts up fine,
  rollback doesn't save it. Put the board type in the app description (or
  version string) at build time and refuse a mismatch on upload.
- **No login on the web UI**: anything on the LAN (or a VPN) can change
  settings; only `/api/prompt_requests` asks for the setup password. An
  optional password would also close DNS rebinding.
- **Self-test ringer**: its fixed 4 s ring ends with `ag1170_ring_stop()`,
  which would also stop a real incoming call's ring that started meanwhile.
- **RTP** accepts audio from any source address and ignores the header's
  extension and padding bits (harmless with Asterisk).
- **Small input nits**: an SSID over 32 characters is silently truncated
  when saved; huge "seconds" values can wrap when multiplied by 1000.

## Improvement suggestions

1. **Split `webui.c`** (8,400 lines) into one file per settings tab plus a
   shared helpers file. The copy-paste is gone (`appendf`, `read_form`,
   `redirect_to`); the size is what's left.
2. **Host-side unit tests** for the pure-logic modules -- dial plan
   collector, SIP header parsing, multipart parser, URL decoding -- compiled
   for the Mac. The bugs found here (truncate-then-decode twice, missing
   header handling) are exactly what such tests catch.
3. **A CI build of every board variant** (SMD2, SMD4, SMD5, SMD5 clone,
   8 MB) on each push, plus cppcheck. A build directory nobody has
   regenerated lately can drift silently today.
4. **Compiler optimization**: the firmware is built `-Og` (debug). `-Os`
   would free a lot of flash and run faster; needs a full hardware retest.
5. **Version the saved settings blobs** (a schema version key next to each)
   before the dial-plan redesign, so old saves can be migrated rather than
   discarded.
