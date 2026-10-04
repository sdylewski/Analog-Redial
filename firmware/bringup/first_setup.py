#!/usr/bin/env python3
"""Bring up and provision a freshly flashed board, one gated step at a time.

Each step only runs if the steps it depends on passed, and the run ends with
a PASS / WARN / FAIL / SKIP summary:

  chip       (opt-in, --chip-info) read chip, flash size and MAC with esptool,
             before anything else touches the port
  console    serial REPL answers; reads the firmware's board type
  codec      the audio codec answers on I2C            <- gates the audio tests
  network    the board answers HTTP at --host          <- gates everything below
  hardware   set lines / microSD / Bluetooth (only if you pass --lines etc.)
  microsd    card present? then write+read+verify it   (skipped on 2-line boards,
             which use the microSD pins for line 2)
  audio      generate and upload the default prompts:
               operator.wav          dial 0
               record_prompt.wav     dial 211, before the beep
               am_greeting.wav       answering machine's default greeting
               digit_0..9.wav        (opt-in, --digits)
  verify     read every uploaded file back from the board
  tests      (opt-in, --tests) the interactive hardware checklist

Serial steps need --port (or exactly one board plugged in); HTTP steps need
--host. Either half can run without the other.

HTTP goes through `curl`, not Python: on macOS, Python's sockets to a LAN
address are blocked unless the terminal has Local Network permission, and
curl isn't affected.

Run it from an ESP-IDF shell (`. ~/esp/esp-idf/export.sh`) -- that Python
has pyserial and esptool.

Examples
--------
  # everything, board on USB and on Wi-Fi
  ./first_setup.py --host telephono.local

  # what chip and flash size is this? (nothing else)
  ./first_setup.py --chip-info --only chip

  # two-line board in German, record the config too
  ./first_setup.py --host 192.168.0.123 --lines 2 --language de

  # just redo the prompts
  ./first_setup.py --host telephono.local --only audio verify
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

STEP_NAMES = ("chip", "console", "codec", "network", "hardware",
              "microsd", "audio", "verify", "tests")

# Default prompt wording per language. English is the reference; the others
# are plain translations of the same sentences, so a builder can diff them.
# The operator message is assembled from parts, because what it should say
# depends on how the board is built -- no point telling someone to dial 321
# on a single-line phone.
PROMPTS = {
    "en": {
        "record": "Record your message after the beep, then hang up.",
        "greeting": "Leave a message after the beep.",
        "op_second_line": "Dial 3 2 1 to call the other phone.",
        "op_record": "Dial 2 1 1 to record a message.",
        "op_bluetooth": "If your phone is paired with this device, you can "
                        "receive calls, and call out by dialing 1 and then the number.",
        "digits": ["zero", "one", "two", "three", "four",
                   "five", "six", "seven", "eight", "nine"],
    },
    "de": {
        "record": "Sprechen Sie Ihre Nachricht nach dem Signalton und legen Sie dann auf.",
        "greeting": "Hinterlassen Sie eine Nachricht nach dem Signalton.",
        "op_second_line": "Wählen Sie 3 2 1, um das andere Telefon anzurufen.",
        "op_record": "Wählen Sie 2 1 1, um eine Nachricht aufzunehmen.",
        "op_bluetooth": "Wenn Ihr Telefon mit diesem Gerät gekoppelt ist, können Sie "
                        "Anrufe empfangen und wählen, indem Sie 1 und dann die Nummer wählen.",
        "digits": ["null", "eins", "zwei", "drei", "vier",
                   "fünf", "sechs", "sieben", "acht", "neun"],
    },
    "fr": {
        "record": "Enregistrez votre message après le bip, puis raccrochez.",
        "greeting": "Laissez un message après le bip.",
        "op_second_line": "Composez le 3 2 1 pour appeler l'autre téléphone.",
        "op_record": "Composez le 2 1 1 pour enregistrer un message.",
        "op_bluetooth": "Si votre téléphone est appairé avec cet appareil, vous pouvez "
                        "recevoir des appels et appeler en composant le 1 puis le numéro.",
        "digits": ["zéro", "un", "deux", "trois", "quatre",
                   "cinq", "six", "sept", "huit", "neuf"],
    },
    "it": {
        "record": "Registra il tuo messaggio dopo il segnale acustico, poi riaggancia.",
        "greeting": "Lascia un messaggio dopo il segnale acustico.",
        "op_second_line": "Componi 3 2 1 per chiamare l'altro telefono.",
        "op_record": "Componi 2 1 1 per registrare un messaggio.",
        "op_bluetooth": "Se il tuo telefono è associato a questo dispositivo, puoi ricevere "
                        "chiamate e chiamare componendo 1 e poi il numero.",
        "digits": ["zero", "uno", "due", "tre", "quattro",
                   "cinque", "sei", "sette", "otto", "nove"],
    },
    "es": {
        "record": "Grabe su mensaje después de la señal y luego cuelgue.",
        "greeting": "Deje un mensaje después de la señal.",
        "op_second_line": "Marque 3 2 1 para llamar al otro teléfono.",
        "op_record": "Marque 2 1 1 para grabar un mensaje.",
        "op_bluetooth": "Si su teléfono está emparejado con este dispositivo, puede recibir "
                        "llamadas y llamar marcando 1 y luego el número.",
        "digits": ["cero", "uno", "dos", "tres", "cuatro",
                   "cinco", "seis", "siete", "ocho", "nueve"],
    },
}

# Same I2C addresses board.c's codec_detect checks.
CODECS = {0x10: "ES8388", 0x11: "ES8388"}


# ---------------------------------------------------------------- results

@dataclass
class Result:
    step: str
    status: str   # PASS WARN FAIL SKIP
    detail: str


class Run:
    def __init__(self, only: set[str] | None):
        self.only = only
        self.results: list[Result] = []

    def wanted(self, step: str) -> bool:
        return self.only is None or step in self.only

    def record(self, step: str, status: str, detail: str) -> str:
        self.results.append(Result(step, status, detail))
        mark = {"PASS": "ok", "WARN": "!!", "FAIL": "XX", "SKIP": "--"}[status]
        print(f"  [{mark}] {status}: {detail}")
        return status

    def header(self, step: str, title: str) -> None:
        print(f"\n== {step}: {title}")

    def summary(self) -> int:
        print("\n" + "=" * 64)
        width = max(len(r.step) for r in self.results) if self.results else 8
        for r in self.results:
            print(f"  {r.step:<{width}}  {r.status:<4}  {r.detail}")
        print("=" * 64)
        failed = [r for r in self.results if r.status == "FAIL"]
        if failed:
            print(f"{len(failed)} step(s) failed.")
            return 1
        print("No failures.")
        return 0


# ---------------------------------------------------------------- HTTP via curl

def _url(host: str, path: str) -> str:
    base = host if host.startswith(("http://", "https://")) else f"http://{host}"
    return base.rstrip("/") + path


def curl(host: str, path: str, *, form: dict[str, str] | None = None,
         wav: Path | None = None, timeout: int = 20) -> tuple[int, bytes]:
    """One request through curl. Returns (HTTP status, body); status 0 = no
    connection. Form handlers answer with a 303 redirect, which counts as
    success -- it isn't followed."""
    with tempfile.NamedTemporaryFile() as body:
        cmd = ["curl", "-sS", "-m", str(timeout), "-o", body.name, "-w", "%{http_code}"]
        if form is not None:
            for k, v in form.items():
                cmd += ["--data-urlencode", f"{k}={v}"]
        if wav is not None:
            cmd += ["-H", "Content-Type: audio/wav", "--data-binary", f"@{wav}"]
        cmd.append(_url(host, path))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        try:
            code = int(proc.stdout.strip() or 0)
        except ValueError:
            code = 0
        return code, Path(body.name).read_bytes()


def ok_status(code: int) -> bool:
    return 200 <= code < 400


# ---------------------------------------------------------------- steps

def step_chip(run: Run, port: str | None) -> dict:
    run.header("chip", "chip and flash (esptool, no firmware needed)")
    if not port:
        run.record("chip", "SKIP", "no serial port")
        return {}
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "-p", port, "flash_id"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    info = {
        "chip": _grab(r"Chip is (.+)", out),
        "mac": _grab(r"MAC: ([0-9a-f:]+)", out),
        "flash": _grab(r"Detected flash size: (\S+)", out),
    }
    if proc.returncode != 0 or not info["flash"]:
        run.record("chip", "FAIL", "esptool couldn't read the chip -- port busy (a monitor "
                   "still open?) or the board stuck; try again or re-plug USB")
        return {}
    detail = f"{info['chip']}, flash {info['flash']}, MAC {info['mac']}"
    if info["flash"] != "16MB":
        run.record("chip", "WARN", detail + " -- the standard build assumes 16 MB; an "
                   "8 MB part needs the 8 MB partition table (docs/bom.md, 'Flash size')")
    else:
        run.record("chip", "PASS", detail)
    print("  (esptool reset the board; waiting for it to boot)")
    return info


def _grab(pattern: str, text: str) -> str:
    m = re.search(pattern, text)
    return m.group(1).strip() if m else ""


def open_console(port: str):
    from phone_serial import PhoneConsole
    con = PhoneConsole(port)
    con.__enter__()
    if not con.is_alive():
        con.wait_for_boot(timeout_s=20)
        if not con.is_alive():
            con.__exit__(None, None, None)
            return None
    return con


def step_console(run: Run, con) -> dict:
    run.header("console", "serial console")
    if con is None:
        run.record("console", "FAIL", "no prompt from the board -- flashed? right port? "
                   "close any idf.py monitor first. Idle 5+ min? It may be asleep in "
                   "low-power idle: press the board's RESET button and retry")
        return {}
    lines = con.send("board_info")
    info = {}
    for line in lines:
        if ":" in line:
            k, v = line.split(":", 1)
            info[k.strip()] = v.strip()
    if "board" not in info:
        run.record("console", "FAIL", f"board_info didn't answer as expected: {lines[:3]}")
        return {}
    pcb = info.get("pcb", info["board"])
    rec = info.get("pcb_recorded", "-")
    note = f" (but this board last confirmed as {rec} -- wrong firmware?)" if rec not in ("-", pcb) else ""
    run.record("console", "WARN" if note else "PASS", f"firmware built for {pcb}{note}")
    return info


def step_codec(run: Run, con) -> bool:
    run.header("codec", "audio codec on I2C")
    lines = con.send("i2c_scan", timeout_s=8)
    found = [int(m.group(1), 16) for l in lines for m in [re.search(r"found 0x([0-9A-Fa-f]{2})", l)] if m]
    codecs = [f"{CODECS[a]} @ 0x{a:02X}" for a in found if a in CODECS]
    if not codecs:
        run.record("codec", "FAIL", f"no codec on I2C (saw {[hex(a) for a in found] or 'nothing'})"
                   " -- check the codec's solder joints and 3.3 V")
        return False
    run.record("codec", "PASS", ", ".join(codecs))
    return True


def step_network(run: Run, host: str | None) -> dict:
    run.header("network", "board on the network")
    if not host:
        run.record("network", "SKIP", "no --host given")
        return {}
    # A board formatting its storage partition for the first time (a new
    # board's first-ever flash) can take up to a minute to get that far --
    # see flash_board.py's matching comment. Retry rather than fail once.
    deadline = time.time() + 75
    code, body = curl(host, "/api/status", timeout=10)
    while not ok_status(code) and time.time() < deadline:
        code, body = curl(host, "/api/status", timeout=10)
    if not ok_status(code):
        run.record("network", "FAIL", f"no answer from {host} -- is it on Wi-Fi? Try its IP "
                   "instead of .local, or hold BOOT ~3 s for the rescue Wi-Fi")
        return {}
    try:
        st = json.loads(body)
    except json.JSONDecodeError:
        run.record("network", "FAIL", f"{host} answered, but not with status JSON")
        return {}
    run.record("network", "PASS", f"'{st.get('board_name')}' {st.get('hardware', '')}, "
               f"{st.get('wifi_ip')} on '{st.get('wifi_ssid')}' ({st.get('wifi_rssi')} dBm)")
    return st


def step_hardware(run: Run, host: str, args, status: dict) -> dict:
    run.header("hardware", "lines / microSD / Bluetooth")
    if args.lines is None and not args.bluetooth:
        n = len(status.get("lines", []))
        run.record("hardware", "SKIP", f"left as is: {n} line(s), SD "
                   f"{'in use' if status.get('storage_on_sd') else 'not in use'} "
                   "(pass --lines to change)")
        return status
    lines = args.lines or len(status.get("lines", [])) or 1
    variant = "2line" if lines == 2 else ("1line_nosd" if args.no_microsd else "1line_sd")
    form = {"lines": variant}
    if args.bluetooth:
        form["bt"] = "1"
    code, _ = curl(host, "/board_hw_set", form=form)
    if not ok_status(code):
        run.record("hardware", "FAIL", f"board refused '{variant}' (HTTP {code})")
        return status
    _, body = curl(host, "/api/status")
    status = json.loads(body) if body else status
    run.record("hardware", "PASS", f"set to {variant}{' + Bluetooth' if args.bluetooth else ''}"
               " (line/SD changes take effect after a reboot)")
    return status


def step_microsd(run: Run, con, status: dict) -> None:
    run.header("microsd", "microSD card")
    nlines = len(status.get("lines", [])) if status else 0
    if nlines == 2:
        run.record("microsd", "SKIP", "2-line board: line 2 uses the microSD pins")
        return
    if status and not status.get("storage_on_sd"):
        run.record("microsd", "WARN", "no card in use -- none inserted, not FAT-formatted, or "
                   "the board isn't set to single line + microSD. Recordings will use "
                   "internal flash (~4 MB)")
        return
    if con is None:
        run.record("microsd", "PASS" if status else "SKIP",
                   "card mounted; pass --port to also run the write/read test"
                   if status else "no serial port and no --host: can't check")
        return
    out = con.send("sd_test", timeout_s=30)
    text = " ".join(out)
    if "write+read+verify OK" in text:
        speed = _grab(r"OK \((.+)\)", text)
        name = _grab(r"Name: (\S+)", text)
        run.record("microsd", "PASS", f"card {name} write/read/verify OK ({speed})".replace("  ", " "))
    elif "isn't configured for microSD" in text:
        run.record("microsd", "WARN", "board isn't set to single line + microSD "
                   "(Settings -> Hardware, or --lines 1)")
    elif "not mounted" in text or "mount failed" in text:
        run.record("microsd", "WARN", "no card found -- inserted? FAT32-formatted?")
    else:
        run.record("microsd", "FAIL", f"card found but the self-test failed: {text[:160]}")


def operator_text(lang: str, lines: int, bluetooth: bool) -> str:
    p = PROMPTS[lang]
    parts = []
    if lines == 2:
        parts.append(p["op_second_line"])
    parts.append(p["op_record"])
    if bluetooth:
        parts.append(p["op_bluetooth"])
    return " ".join(parts)


def generate(text: str, name: str, out_dir: Path, extra: list[str]) -> Path | None:
    cmd = [sys.executable, str(HERE / "gen_prompt.py"), text,
           "--name", name, "--out-dir", str(out_dir), *extra]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    path = out_dir / name
    if proc.returncode != 0 or not path.exists():
        print(proc.stdout[-400:], proc.stderr[-400:], sep="\n")
        return None
    return path


def step_audio(run: Run, host: str, args, status: dict) -> list[str]:
    run.header("audio", f"default prompts ({args.language})")
    p = PROMPTS[args.language]
    nlines = len(status.get("lines", [])) or 1
    bt = bool(args.bluetooth or status.get("bt_enabled"))
    clips = [
        ("operator.wav", operator_text(args.language, nlines, bt), True),
        ("record_prompt.wav", p["record"], True),
    ]
    # The shared default greeting, not per-line am_greeting_l<N>.wav -- those
    # are each line's own recording (dial 899) and override this one.
    clips.append(("am_greeting.wav", p["greeting"], True))
    if args.digits:
        clips += [(f"digit_{i}.wav", w, False) for i, w in enumerate(p["digits"])]

    extra = []
    if args.engine:
        extra += ["--engine", args.engine]
    if args.voice:
        extra += ["--voice", args.voice]

    uploaded = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, text, track_text in clips:
            print(f"  {name}: {text}")
            wav = generate(text, name, Path(tmp), extra)
            if wav is None:
                run.record("audio", "FAIL", f"{name}: speech generation failed (see above)")
                continue
            code, body = curl(host, f"/media/{name}", wav=wav, timeout=60)
            if not ok_status(code):
                run.record("audio", "FAIL", f"{name}: upload refused (HTTP {code}) "
                           f"{body[:80].decode(errors='replace')}")
                continue
            if track_text:
                # Lets the web UI show what the clip says, so it can be edited.
                curl(host, "/prompt_text_set", form={"name": name, "text": text})
            uploaded.append(name)
    if uploaded:
        run.record("audio", "PASS" if len(uploaded) == len(clips) else "WARN",
                   f"uploaded {len(uploaded)}/{len(clips)}: {', '.join(uploaded)}")
    return uploaded


def step_verify(run: Run, host: str, names: list[str]) -> None:
    run.header("verify", "read the uploads back")
    if not names:
        run.record("verify", "SKIP", "nothing was uploaded")
        return
    bad = []
    for name in names:
        code, body = curl(host, f"/media/{name}", timeout=30)
        if code != 200 or not body.startswith(b"RIFF"):
            bad.append(f"{name} (HTTP {code})")
    if bad:
        run.record("verify", "FAIL", "not readable back: " + ", ".join(bad))
    else:
        run.record("verify", "PASS", f"all {len(names)} file(s) read back as WAV")


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="Board hostname or IP, e.g. telephono.local")
    ap.add_argument("--port", help="Serial port (default: the one board plugged in)")
    ap.add_argument("--no-serial", action="store_true", help="Skip every serial step")
    ap.add_argument("--chip-info", action="store_true",
                    help="Read chip/flash/MAC with esptool first (resets the board)")
    ap.add_argument("--only", nargs="+", choices=STEP_NAMES, metavar="STEP",
                    help=f"Run just these steps: {' '.join(STEP_NAMES)}")
    ap.add_argument("--language", default="en", choices=sorted(PROMPTS))
    ap.add_argument("--lines", type=int, choices=(1, 2),
                    help="Set the board to 1 or 2 lines (default: leave as is)")
    ap.add_argument("--no-microsd", action="store_true",
                    help="With --lines 1: no microSD socket fitted")
    ap.add_argument("--bluetooth", action="store_true", help="BM64 companion fitted")
    ap.add_argument("--digits", action="store_true",
                    help="Also upload spoken digits 0-9 (for digit readback)")
    ap.add_argument("--tests", action="store_true",
                    help="Finish with the interactive hardware checklist")
    ap.add_argument("--engine", help="gen_prompt.py speech engine (piper/espeak/say)")
    ap.add_argument("--voice", help="gen_prompt.py voice")
    args = ap.parse_args()

    if args.lines == 2 and args.no_microsd:
        ap.error("--no-microsd only means something with --lines 1")
    if not shutil.which("curl"):
        ap.error("curl not found")

    if args.only:
        only = set(args.only)
    else:
        only = set(STEP_NAMES) - {"chip", "tests"}
        if args.chip_info:
            only.add("chip")
        if args.tests:
            only.add("tests")
    run = Run(only)

    port = None
    if not args.no_serial:
        from phone_serial import find_board_ports
        ports = find_board_ports()
        port = args.port or (ports[0] if len(ports) == 1 else None)
        if not port and not args.port and len(ports) > 1:
            print("Several boards plugged in -- pass --port. Serial steps skipped.")

    if run.wanted("chip"):
        step_chip(run, port)

    con = None
    serial_steps = {"console", "codec", "microsd", "tests"} & only
    if port and serial_steps:
        con = open_console(port)

    try:
        codec_ok = True
        if run.wanted("console"):
            if port:
                if not step_console(run, con):
                    con = None
            else:
                run.header("console", "serial console")
                run.record("console", "SKIP", "no serial port")
        if run.wanted("codec"):
            if con is not None:
                codec_ok = step_codec(run, con)
            else:
                run.header("codec", "audio codec on I2C")
                run.record("codec", "SKIP", "needs the serial console")

        status: dict = {}
        if only & {"network", "hardware", "microsd", "audio", "verify"}:
            status = step_network(run, args.host)
        online = bool(status)

        if run.wanted("hardware"):
            if online:
                status = step_hardware(run, args.host, args, status)
            else:
                run.header("hardware", "lines / microSD / Bluetooth")
                run.record("hardware", "SKIP", "board not reachable over HTTP")

        if run.wanted("microsd"):
            step_microsd(run, con, status)

        uploaded: list[str] = []
        if run.wanted("audio"):
            if online:
                uploaded = step_audio(run, args.host, args, status)
            else:
                run.header("audio", "default prompts")
                run.record("audio", "SKIP", "board not reachable over HTTP")
        if run.wanted("verify"):
            if online:
                names = uploaded or (["operator.wav", "record_prompt.wav", "am_greeting.wav"]
                                     if not run.wanted("audio") else [])
                step_verify(run, args.host, names)
            else:
                run.header("verify", "read the uploads back")
                run.record("verify", "SKIP", "board not reachable over HTTP")

        if run.wanted("tests"):
            run.header("tests", "hardware checklist")
            if con is None or not codec_ok:
                run.record("tests", "SKIP", "needs a working console and codec")
            else:
                con.__exit__(None, None, None)   # the checklist opens the port itself
                con = None
                rc = subprocess.call([sys.executable, str(HERE / "bringup_checklist.py"),
                                      "--port", port])
                run.record("tests", "PASS" if rc == 0 else "FAIL",
                           "checklist completed" if rc == 0 else f"checklist stopped (exit {rc})")
    finally:
        if con is not None:
            con.__exit__(None, None, None)

    return run.summary()


if __name__ == "__main__":
    sys.exit(main())
