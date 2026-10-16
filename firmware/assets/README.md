# firmware/assets/ — bundled audio (baked in at build/flash time)

The files listed in `BUNDLED_AUDIO` in `firmware/main/CMakeLists.txt` —
**`operator.wav`, `record_prompt.wav`, `am_greeting.wav`, `dialup.wav` and
`jenny.wav` and the spoken digits `0.wav`-`9.wav` and `point.wav`, nothing else** — get
baked into a **read-only-in-spirit `bundled` SPIFFS partition** at build time
and flashed alongside the firmware image (see the `bundled` entry in
`firmware/partitions.csv`). Anything else you keep in this directory
(personal clips, draft translations in `i18n/`) stays on your disk and is
**not** built into boards. It's a *separate* partition
from `storage` (recordings + anything uploaded over HTTP at runtime), so:

- A normal `idf.py flash` re-bakes and reflashes those files, but **never
  touches recordings** or uploaded clips in `storage`. It *does* replace the
  whole `bundled` partition, so clips an older build baked in are gone after
  the next flash unless copied out first: run
  `bringup/keep_bundled_audio.py --host <board>` before reflashing an older
  board (it copies them into `storage`, where they keep working).
- Uploading a same-named file to `storage` (curl / `gen_prompt.py` /
  the web UI's Storage files section) **overrides** a bundled default of the
  same name, without a rebuild — `storage.c` checks `storage` first, falls
  back to `bundled`.

## What's shipped by default

**`operator.wav`** — synthesized from `pi_rotary_phone/operator_help_en.txt`
(original project text, not copyrighted). Dial `0` to hear it.

**`record_prompt.wav`** — "Record your message after the beep, then hang
up.", generated with `bringup/gen_prompt.py` from the same English text
`first_setup.py` uses. Plays before the beep when you dial `211`.

**`am_greeting.wav`** — the answering machine's default greeting, "Leave a
message after the beep.", generated with `bringup/gen_prompt.py`. Plays on
any line that has no recorded greeting of its own (`am_greeting_l<N>.wav`,
recorded by dialling 899). Replace the wording from Settings -> Phone
Settings -> Answering machine -> Default greeting (text-to-speech), which
uploads a same-named file that overrides this one.

Two "novelty clip" defaults ship out of the box: 412 (below) and 8675309. (402 Burp,
401/403/404 Ring of Fire / Crocodile Rock / Martians, and 411/413/414 were
all retired from the defaults -- personal or copyrighted audio, or no
source audio at all. Boards provisioned earlier keep those codes.)

**`0.wav` ... `9.wav`, `point.wav`** -- the digits spoken one at a time
(about 0.4 s each, 8 kHz 8-bit µ-law, 38 KB in all), made with
`bringup/gen_prompt.py` (Piper, the same voice as the other prompts) and
trimmed with `ffmpeg`. Digit readback (dial 822) says each digit you dial with
them, and dial 314 reads pi aloud: "three point one four one five...", the first
100 decimals. Without them readback falls back to a touch-tone burst. Replace a
digit by uploading a same-named file under Settings -> Storage.

**`jenny.wav`** (8675309, "Jenny") -- "If you're trying to reach Jenny, she's
not home right now. Please call back later.", an original message synthesized
with `bringup/gen_prompt.py` (Piper) and stored as 8 kHz 8-bit µ-law WAV
(38 KB; same `ffmpeg` command as below). It's a plain spoken line: no music,
nothing from any recording. Boards set up before it existed get the code added
at their next boot.

**`dialup.wav`** (412, "Dial up modem") — public domain
(Wikimedia Commons, `Dial_up_connection.ogg`), full length (~39 s), stored
as **8 kHz 8-bit µ-law WAV**: 314 KB, half the size of 16-bit PCM (630 KB,
which wouldn't fit an 8 MB board's 1 MB `bundled` partition alongside the
other clips) and the same quality a phone line carries anyway. The player
plays mono 16-bit PCM and mono 8-bit µ-law WAVs. To make one:

```bash
ffmpeg -i in.wav -ac 1 -ar 8000 -c:a pcm_mulaw -map_metadata -1 -fflags +bitexact out.wav
```

MP3 clips still play (`CONFIG_PLAYER_MP3`; any `--name` ending in `.mp3`
in `gen_prompt.py`), but each one needs a 28 KB task stack plus an 8 KB
buffer while it plays, against about 6 KB for a WAV. Prefer µ-law WAV for
anything long.

## Adding your own (fun clips, other prompts)

Codes with no source audio aren't bundled **on purpose**, same as when the
source is a copyrighted commercial song that doesn't belong in a public repo
or a build anyone can download. To add your own, from audio you own the
rights to:

```bash
cd firmware/bringup
./sync_pi_audio.py --src /path/to/your/audio --out-dir ../assets
# or convert one file at a time:
./gen_prompt.py --from your_song.mp3 --name your_song.wav --out-dir ../assets
```

Everything except the three shipped files is gitignored — see
`.gitignore` — so your personal clips never get committed. Well-known
filenames the firmware looks for: `operator.wav` (dial 0, fixed) and
whatever `dialplan_defaults.h`'s `CLIP_DEFAULTS` currently points each 4xx
code at (not a fixed naming convention any more -- both the code and the
filename are freely editable at runtime from Settings → Dial Codes). To
assign a *new* dial code to a clip instead of replacing an existing one,
upload the file (as above or via Settings → Storage) then add the code
under Settings → Dial Codes — no firmware rebuild needed for that part.

Files you add here are **not** built in automatically any more. Upload
them to a board instead (Settings → Storage, or `gen_prompt.py --upload`),
or, to ship one with every build, add it to `BUNDLED_AUDIO` in
`firmware/main/CMakeLists.txt`.
