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

Only **`operator.wav`** — synthesized from `pi_rotary_phone/operator_help_en.txt`
(original project text, not copyrighted). Dial `0` to hear it.

## Adding your own (fun clips, other prompts)

Nothing else is bundled by default **on purpose**: the clips this project
was tested against (Ring of Fire, Crocodile Rock, Banana Boat, ...) are
copyrighted commercial songs and don't belong in a public repo or a build
anyone can download. To add your own, from audio you own the rights to:

```bash
cd firmware/bringup
./sync_pi_audio.py --src /path/to/your/audio --out-dir ../assets
# or convert one file at a time:
./gen_prompt.py --from your_song.mp3 --name clip_401.wav --out-dir ../assets
```

Everything except `operator.wav` in this directory is gitignored — see
`.gitignore` — so your personal clips never get committed. Well-known
filenames the firmware looks for: `operator.wav` (dial 0), `clip_401.wav`
.. `clip_404.wav` and `clip_411.wav` .. `clip_414.wav` (silly clips, see
`pi_rotary_phone/docs/DIAL_CODES.md`). To assign a *new* dial code to a
clip instead of replacing an existing one, upload the file (as above or via
Settings → Storage) then add the code under Settings → General → "Custom
dial codes" — no firmware rebuild needed for that part.

Once you've added files here, they're included automatically the next time
you run `idf.py flash` — no extra command.
