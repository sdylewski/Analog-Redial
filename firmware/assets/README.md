# firmware/assets/ — bundled audio (baked in at build/flash time)

Every WAV file here gets baked into a **read-only-in-spirit `bundled` SPIFFS
partition** at build time and flashed alongside the firmware image (see
`spiffs_create_partition_image` in `firmware/main/CMakeLists.txt`, and the
`bundled` entry in `firmware/partitions.csv`). It's a *separate* partition
from `storage` (recordings + anything uploaded over HTTP at runtime), so:

- A normal `idf.py flash` re-bakes and reflashes this directory's contents,
  but **never touches recordings** in `storage`.
- Uploading a same-named file to `storage` (curl / `gen_prompt.py` /
  the web UI's Storage files section) **overrides** a bundled default of the
  same name, without a rebuild — `storage.c` checks `storage` first, falls
  back to `bundled`.

## What's shipped by default

**`operator.wav`** — synthesized from `pi_rotary_phone/operator_help_en.txt`
(original project text, not copyrighted). Dial `0` to hear it.

**`am_greeting.wav`** — the answering machine's default greeting, "Leave a
message after the beep.", generated with `bringup/gen_prompt.py`. Plays on
any line that has no recorded greeting of its own (`am_greeting_l<N>.wav`,
recorded by dialling 899). Replace the wording from Settings -> Phone
Settings -> Answering machine -> Default greeting (text-to-speech), which
uploads a same-named file that overrides this one.

Two 4xx "novelty clip" defaults remain, out of the box:

**`burp.wav`** (402, "Burp") — sourced from `pi_rotary_phone/Audio_files/`
(personal library, not a copyrighted-song original redistributed elsewhere)
and trimmed to 20s at 8kHz mono. (401/403/404 -- Ring of Fire / That's
Amore, Crocodile Rock / Crocfa, Martians -- and 411/413/414 -- Banana
Phone/Split, Lime in the Coconut -- were all retired: the former traded
away for a smaller default set ahead of public release, the latter because
no source audio for them ever existed in `Audio_files/`.)

**`dialup.mp3`** (412, "Dial up modem") — public domain
(Wikimedia Commons, `Dial_up_connection.ogg`), full length (~39s) rather
than trimmed, encoded as MP3 instead of WAV -- see `CONFIG_PLAYER_MP3`
(`components/player/Kconfig`) and `gen_prompt.py`'s `convert_to_mp3()`.
A full song at 8kHz/16-bit PCM WAV runs 2-3MB; the same audio as ~40kbps
mono MP3 is 3-4x smaller, which is the difference between "fits" and
"doesn't" when clips share a few MB of flash with recordings. Any `--name`
ending in `.mp3` (one-shot `gen_prompt.py`, or `Settings -> Dial Codes` /
`Operator` uploads) gets this treatment automatically; `.wav` is unaffected
and stays raw PCM.

## Adding your own (fun clips, other prompts)

Codes with no source audio aren't bundled **on purpose**, same as when the
source is a copyrighted commercial song that doesn't belong in a public repo
or a build anyone can download. To add your own, from audio you own the
rights to:

```bash
cd firmware/bringup
./sync_pi_audio.py --src /path/to/your/audio --out-dir ../assets
# or convert one file at a time (name it *.mp3 for the compressed path):
./gen_prompt.py --from your_song.mp3 --name your_song.mp3 --out-dir ../assets
```

Everything except `operator.wav` in this directory is gitignored — see
`.gitignore` — so your personal clips never get committed. Well-known
filenames the firmware looks for: `operator.wav` (dial 0, fixed) and
whatever `dialplan_defaults.h`'s `CLIP_DEFAULTS` currently points each 4xx
code at (not a fixed naming convention any more -- both the code and the
filename are freely editable at runtime from Settings → Dial Codes). To
assign a *new* dial code to a clip instead of replacing an existing one,
upload the file (as above or via Settings → Storage) then add the code
under Settings → Dial Codes — no firmware rebuild needed for that part.

Once you've added files here, they're included automatically the next time
you run `idf.py flash` — no extra command.
