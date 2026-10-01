#!/usr/bin/env python3
"""One-shot first setup for a freshly flashed board.

Everything a new board needs after `idf.py flash`, in one command, run from
the computer rather than typed into the console:

  1. find the board and check it answers
  2. optionally run the hardware bring-up checklist
  3. tell it what hardware it has (lines, microSD, Bluetooth)
  4. generate the default spoken prompts for the chosen language and upload
  5. generate spoken digits 0-9 for digit readback and upload

Each step is skippable, and every step is idempotent -- re-running it is
safe and is the normal way to change one thing later.

This orchestrates the existing tools rather than reimplementing them:
gen_prompt.py does the text-to-speech and upload, bringup_checklist.py does
the hardware tests. Run those directly when you want one piece on its own.

Examples
--------
  # everything, board found by mDNS name
  ./first_setup.py --host newphone.local

  # two-line board with Bluetooth, in German, skipping the hardware tests
  ./first_setup.py --host 192.168.0.123 --lines 2 --bluetooth \\
                   --language de --skip-tests

  # just regenerate the prompts after editing the default text
  ./first_setup.py --host newphone.local --only prompts
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
TIMEOUT = 20

# Default prompt wording per language. English is the reference; the others
# are deliberately plain translations of the same sentence rather than
# localised rewrites, so a builder can diff them.
#
# The operator message is assembled from parts, because what it should say
# depends on how the board is actually built -- there's no point telling
# someone to dial 321 on a single-line phone.
PROMPTS = {
    "en": {
        "record": "Leave a message after the beep.",
        "op_intro": "",
        "op_second_line": "Dial 3 2 1 to call the other phone.",
        "op_record": "Dial 2 1 1 to leave a message.",
        "op_bluetooth": "If your phone is paired with this device, "
                        "you can receive calls, and call out by dialing 1 "
                        "and then the number.",
        "digits": ["zero", "one", "two", "three", "four",
                   "five", "six", "seven", "eight", "nine"],
    },
    "de": {
        "record": "Hinterlassen Sie eine Nachricht nach dem Signalton.",
        "op_intro": "",
        "op_second_line": "Wählen Sie 3 2 1, um das andere Telefon anzurufen.",
        "op_record": "Wählen Sie 2 1 1, um eine Nachricht zu hinterlassen.",
        "op_bluetooth": "Wenn Ihr Telefon mit diesem Gerät gekoppelt ist, "
                        "können Sie Anrufe empfangen und wählen, "
                        "indem Sie 1 und dann die Nummer wählen.",
        "digits": ["null", "eins", "zwei", "drei", "vier",
                   "fünf", "sechs", "sieben", "acht", "neun"],
    },
    "fr": {
        "record": "Laissez un message après le bip.",
        "op_intro": "",
        "op_second_line": "Composez le 3 2 1 pour appeler l'autre téléphone.",
        "op_record": "Composez le 2 1 1 pour laisser un message.",
        "op_bluetooth": "Si votre téléphone est appairé avec cet appareil, "
                        "vous pouvez recevoir des appels et appeler en composant le 1 "
                        "puis le numéro.",
        "digits": ["zéro", "un", "deux", "trois", "quatre",
                   "cinq", "six", "sept", "huit", "neuf"],
    },
    "it": {
        "record": "Lascia un messaggio dopo il segnale acustico.",
        "op_intro": "",
        "op_second_line": "Componi 3 2 1 per chiamare l'altro telefono.",
        "op_record": "Componi 2 1 1 per lasciare un messaggio.",
        "op_bluetooth": "Se il tuo telefono è associato a questo dispositivo, "
                        "puoi ricevere chiamate e chiamare componendo 1 "
                        "e poi il numero.",
        "digits": ["zero", "uno", "due", "tre", "quattro",
                   "cinque", "sei", "sette", "otto", "nove"],
    },
    "es": {
        "record": "Deje un mensaje después de la señal.",
        "op_intro": "",
        "op_second_line": "Marque 3 2 1 para llamar al otro teléfono.",
        "op_record": "Marque 2 1 1 para dejar un mensaje.",
        "op_bluetooth": "Si su teléfono está emparejado con este dispositivo, "
                        "puede recibir llamadas y llamar marcando 1 "
                        "y luego el número.",
        "digits": ["cero", "uno", "dos", "tres", "cuatro",
                   "cinco", "seis", "siete", "ocho", "nueve"],
    },
}


def post(host: str, path: str, fields: dict[str, str]) -> bool:
    """POST a form to the board. Returns True on 2xx/3xx."""
    url = f"http://{host}{path}"
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            return 200 <= resp.status < 400
    except urllib.error.HTTPError as e:
        # A 303 redirect back to the settings page is the normal success path
        # for these forms; urllib follows it, so an HTTPError here is real.
        print(f"    ! {path} -> HTTP {e.code}", file=sys.stderr)
        return False
    except OSError as e:
        print(f"    ! {path} -> {e}", file=sys.stderr)
        return False


def check_board(host: str) -> bool:
    print(f"[1/5] Checking {host} answers ...")
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=TIMEOUT) as r:
            body = r.read(400).decode(errors="replace")
    except OSError as e:
        print(f"    ! can't reach {host}: {e}", file=sys.stderr)
        print("      Is it on the network? Try the board's IP instead of "
              ".local, or hold BOOT ~3 s for the rescue Wi-Fi.", file=sys.stderr)
        return False
    print(f"    ok -- {body[:120]}")
    return True


def run_hardware_tests() -> bool:
    print("[2/5] Hardware bring-up checklist ...")
    rc = subprocess.call([sys.executable, str(HERE / "bringup_checklist.py")])
    if rc != 0:
        print("    ! a hardware step failed -- fix that before going on,\n"
              "      or re-run with --skip-tests to configure anyway.", file=sys.stderr)
    return rc == 0


def set_hardware(host: str, lines: int, microsd: bool, bluetooth: bool) -> bool:
    print(f"[3/5] Telling the board what it is "
          f"(lines={lines}, microSD={microsd}, Bluetooth={bluetooth}) ...")
    fields = {"lines": "2" if lines == 2 else ("sd" if microsd else "1")}
    if bluetooth:
        fields["bt"] = "1"
    ok = post(host, "/board_hw_set", fields)
    print("    ok" if ok else "    ! failed")
    return ok


def operator_text(lang: str, lines: int, bluetooth: bool) -> str:
    p = PROMPTS[lang]
    parts = [p["op_intro"]]
    if lines == 2:
        parts.append(p["op_second_line"])
    parts.append(p["op_record"])
    if bluetooth:
        parts.append(p["op_bluetooth"])
    return " ".join(x for x in parts if x).strip()


def gen_and_upload(host: str, text: str, name: str, lang: str, extra: list[str]) -> bool:
    cmd = [sys.executable, str(HERE / "gen_prompt.py"), text,
           "--name", name, "--upload", host, *extra]
    return subprocess.call(cmd) == 0


def do_prompts(host: str, lang: str, lines: int, bluetooth: bool,
               extra: list[str], set_text: bool) -> bool:
    print(f"[4/5] Generating prompts in '{lang}' ...")
    p = PROMPTS[lang]
    ok = True

    op = operator_text(lang, lines, bluetooth)
    print(f"    operator: {op}")
    ok &= gen_and_upload(host, op, "operator.wav", lang, extra)

    print(f"    record prompt: {p['record']}")
    ok &= gen_and_upload(host, p["record"], "record_prompt.wav", lang, extra)

    if set_text:
        # Store the source text too, so the web UI shows what was spoken and
        # the owner can edit it without guessing.
        post(host, "/prompt_text_set", {"name": "operator", "text": op})
        post(host, "/prompt_text_set", {"name": "record_prompt", "text": p["record"]})
    return ok


def do_digits(host: str, lang: str, extra: list[str]) -> bool:
    print(f"[5/5] Generating spoken digits 0-9 in '{lang}' ...")
    words = PROMPTS[lang]["digits"]
    ok = True
    for i, w in enumerate(words):
        print(f"    {i}: {w}")
        ok &= gen_and_upload(host, w, f"digit_{i}.wav", lang, extra)
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True,
                    help="Board hostname or IP (e.g. newphone.local)")
    ap.add_argument("--language", default="en", choices=sorted(PROMPTS),
                    help="Language for the generated audio (default: en)")
    ap.add_argument("--lines", type=int, default=1, choices=(1, 2),
                    help="Phone lines fitted (default: 1)")
    ap.add_argument("--microsd", action="store_true",
                    help="microSD fitted instead of line 2 (single-line builds only)")
    ap.add_argument("--bluetooth", action="store_true",
                    help="BM64 Bluetooth companion fitted")
    ap.add_argument("--skip-tests", action="store_true",
                    help="Don't run the hardware bring-up checklist")
    ap.add_argument("--only", choices=("check", "tests", "hardware", "prompts", "digits"),
                    help="Run a single step instead of the whole sequence")
    ap.add_argument("--no-digits", action="store_true",
                    help="Skip the spoken digits (they're only used by digit readback)")
    ap.add_argument("--no-text", action="store_true",
                    help="Upload the audio but don't store the source text on the board")
    ap.add_argument("--engine", help="Pass through to gen_prompt.py (piper/espeak/say)")
    ap.add_argument("--voice", help="Pass through to gen_prompt.py as --voice")
    args = ap.parse_args()

    if args.lines == 2 and args.microsd:
        ap.error("--microsd needs a single-line build: line 2 and the card share GPIO18-21")

    extra: list[str] = []
    if args.engine:
        extra += ["--engine", args.engine]
    if args.voice:
        extra += ["--voice", args.voice]

    only = args.only
    ok = True

    if only in (None, "check") and not check_board(args.host):
        return 1

    if only in (None, "tests") and not args.skip_tests:
        if not run_hardware_tests() and only is None:
            return 1
    elif only is None:
        print("[2/5] Hardware checklist skipped (--skip-tests)")

    if only in (None, "hardware"):
        ok &= set_hardware(args.host, args.lines, args.microsd, args.bluetooth)

    if only in (None, "prompts"):
        ok &= do_prompts(args.host, args.language, args.lines, args.bluetooth,
                         extra, not args.no_text)

    if only in (None, "digits") and not args.no_digits:
        ok &= do_digits(args.host, args.language, extra)
    elif only is None:
        print("[5/5] Digits skipped (--no-digits)")

    print()
    if ok:
        print(f"Done. Open http://{args.host}/ and pick up a handset.")
        return 0
    print("Finished with errors -- see above.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
