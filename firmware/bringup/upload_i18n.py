#!/usr/bin/env python3
"""Sideload a language's UI/prompt string table onto a board at flash/setup
time, instead of bundling every supported language into firmware (see
firmware/components/i18n/include/i18n.h for why: the app partition is small
and already tight, and content that lives on disk here rather than in the
binary costs nothing extra per language on the device).

Reuses the same generic upload endpoint gen_prompt.py --upload already uses
for prompt/clip WAVs (POST /media/<name>) -- no separate firmware endpoint
needed. Files live in firmware/assets/i18n/<lang>.json (see that directory's
README.md for the format and current coverage).

IMPORTANT: as of this writing, i18n_str() (firmware/components/i18n/i18n.c)
is still a stub that always returns its English fallback -- uploading a
language file with this script stages it on the board's storage partition,
but nothing reads it yet. It's here so the sideloading mechanism exists and
is tested before the actual string-table lookup gets wired up, not because
switching languages does anything visible yet.

Usage:
    ./upload_i18n.py it phone.local
    ./upload_i18n.py it phone.local --set-language
    ./upload_i18n.py it phone.local de phone.local --set-language   # (run twice, once per language)
"""

from __future__ import annotations
import argparse
import sys
import urllib.error
import urllib.request
from pathlib import Path

I18N_DIR = Path(__file__).resolve().parent.parent / "assets" / "i18n"


def _auth_headers() -> dict:
    """HTTP Basic auth with the board's web password, if REDIAL_PASSWORD is set."""
    import base64, os
    pw = os.environ.get("REDIAL_PASSWORD")
    if not pw:
        return {}
    return {"Authorization": "Basic " + base64.b64encode(f"admin:{pw}".encode()).decode()}


def _base_url(host: str) -> str:
    if not host.startswith("http://") and not host.startswith("https://"):
        host = f"http://{host}"
    return host.rstrip("/")


def upload_language_file(lang: str, host: str) -> None:
    path = I18N_DIR / f"{lang}.json"
    if not path.exists():
        available = sorted(p.stem for p in I18N_DIR.glob("*.json"))
        raise SystemExit(f"no {path} -- available: {', '.join(available)}")

    data = path.read_bytes()
    name = f"i18n_{lang}.json"
    url = f"{_base_url(host)}/media/{name}"
    req = urllib.request.Request(url, data=data, method="POST",
                                  headers={"Content-Type": "application/json", **_auth_headers()})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            body = resp.read().decode("utf-8", errors="replace").strip()
    except urllib.error.HTTPError as e:
        raise SystemExit(f"upload to {url} failed: HTTP {e.code} {e.reason}") from e
    print(f"uploaded -> {url} ({len(data)} bytes): {body}")


def set_language(lang: str, host: str) -> None:
    url = f"{_base_url(host)}/language_set"
    data = f"lang={lang}".encode("ascii")
    req = urllib.request.Request(url, data=data, method="POST",
                                  headers={"Content-Type": "application/x-www-form-urlencoded",
                                           **_auth_headers()})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            resp.read()
    except urllib.error.HTTPError as e:
        raise SystemExit(f"POST {url} failed: HTTP {e.code} {e.reason}") from e
    print(f"language set -> '{lang}' on {host}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lang", help="Language code (matches a firmware/assets/i18n/<lang>.json file)")
    parser.add_argument("host", help="Board hostname/IP, e.g. phone.local or 192.168.1.42")
    parser.add_argument("--set-language", action="store_true",
                         help="Also select this language on the board (POST /language_set) "
                              "after uploading its file")
    args = parser.parse_args()

    upload_language_file(args.lang, args.host)
    if args.set_language:
        set_language(args.lang, args.host)
    return 0


if __name__ == "__main__":
    sys.exit(main())
