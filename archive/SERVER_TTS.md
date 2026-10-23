# Server-side text-to-speech for spoken prompts (working notes)

Goal: someone handed a board can make its spoken prompts (operator message,
record prompt, answering-machine greeting) from the board's own web page, with
no Mac, Python or script. The board sends the text to the provisioner it
already uses for VoIP numbers; the server renders it and the board saves the
audio.

## Design

- **No new port.** `redial-tts` is a small separate service that listens on
  `127.0.0.1` only. The provisioner forwards `POST /api/v1/tts` to it. A board
  talks to the provisioner's existing address with the key or board token it
  already has.
- **Separate service, not inside the provisioner.** The provisioner stays
  standard-library Python with no dependencies. TTS needs a virtual
  environment (`kokoro-onnx`, `numpy`, `soundfile`) and about 350 MB of model
  files, so it's opt-in: `install.sh --with-tts`.
- **One server-wide voice** (Kokoro `af_sarah`). More voices can come later.
- **Output** is what the board plays best: 8 kHz mono 8-bit mu-law WAV, peak
  just under full scale, leading and trailing silence trimmed. The board
  streams it straight into storage in small chunks (no large buffer).
- **Who may use it:** any request signed with an enrollment key or a board
  token, like `/api/v1/check`. A board's token is tied to its board ID (chip
  serial), so quotas are counted per board; a request signed with the key is
  counted per key.

## Why the board ID alone can't be the key

The board ID is the chip's serial number. It is printed on the Hardware tab,
sent in every request and easy to guess, so it can't prove anything by
itself. The server already does the right thing: the enrollment key admits a
board once, and the server then issues that board its own random token, bound
to its board ID. From then on the token is the board's key. Deleting the
enrollment key doesn't break boards already set up, and `revoke-board` cuts
off one board.

## Limits

- Text up to 500 characters; control characters dropped.
- Quota per board (or per key): 20 renders an hour, 60 a day (config).
- One render at a time; the TTS service is single-threaded, so requests queue.
- Cache keyed by voice and text: the same text again is free.
- The provisioner's existing rate limit and bad-signature lockout still apply.

## Reaching it from outside the LAN (later)

Forward only 443 to the server and put the provisioner behind an HTTPS
reverse proxy (Caddy or nginx) at a path such as `/redial/`, stripping the
prefix. The board signs only the API path (`/api/v1/...`), so a stripped
prefix still verifies. Expose nothing else on 443. VoIP calls from outside
would also need SIP and RTP reachable, which is a much larger exposure (SIP
scanners, toll fraud) and is a separate decision; provisioning and TTS alone
don't need it.
