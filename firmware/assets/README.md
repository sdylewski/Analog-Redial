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

Plus four of the eight 4xx "silly clip" defaults, sourced from
`pi_rotary_phone/Audio_files/` (personal library, not copyrighted-song
originals redistributed elsewhere) and trimmed to 20s at 8kHz mono:
`ring_of_fire.wav` (401), `burp.wav` (402), `crocodile_rock.wav` (403),
`martians.wav` (404). The banana-line codes (411-414: Banana Phone/Boat/
Split, Lime in the Coconut) have no source audio in `Audio_files/` yet — see
`dialplan_defaults.h` for the filenames they expect once you have some.

## Adding your own (fun clips, other prompts)

The banana-line clips above (and anything else you want) aren't bundled
**on purpose** when there's no source for them, or when the source is a
copyrighted commercial song that doesn't belong in a public repo or a build
anyone can download. To add your own, from audio you own the rights to:

```bash
cd firmware/bringup
./sync_pi_audio.py --src /path/to/your/audio --out-dir ../assets
# or convert one file at a time:
./gen_prompt.py --from your_song.mp3 --name banana_phone.wav --out-dir ../assets
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
