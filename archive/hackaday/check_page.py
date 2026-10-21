#!/usr/bin/env python3
"""Read-only checker for a hackaday.io project page, using the official API.

Fetches your project and its logs (GET only; nothing is ever changed) and
looks for the mistakes that are easy to miss in the web editor:

  - the summary being over Hackaday's 140 characters
  - repeated words ("the the"), double spaces, a missing space after a full stop
  - a paragraph or heading that appears twice (a paste slip)
  - leftover notes: [YOU ...], TODO, XXX
  - links that don't load
  - spelling (needs `aspell` or `hunspell` installed; skipped if neither is)
  - differences from your draft, archive/hackaday/hackaday-page.md

SETUP (once)
  1. Log in at https://dev.hackaday.io and register an application. That gives
     you an API key (use the one labelled "API Key", not the client secret).
     A key alone only sees PUBLISHED projects. For a draft, either publish it,
     or copy its text into a file and use --text (no key needed). (The docs don't spell out the button names; look for
     "Apps", "Applications" or "Create app" after logging in.)
  2. Save the key in a file OUTSIDE the repo, readable only by you:
         mkdir -p ~/.config/hackaday && cd ~/.config/hackaday
         umask 077 && read -r -s -p "API key: " K && echo "$K" > api_key
     (or export HACKADAY_API_KEY in your shell). Never paste it into chat or
     commit it. This script never prints it.
  3. Find your project's number: it's in its web address,
     hackaday.io/project/<NUMBER>-name.

RUN
  ./check_page.py --project 206945                 # or the full project URL
  ./check_page.py --project 123456 --dump page.json   # also save the raw reply
  ./check_page.py --offline page.json              # re-check a saved reply
  ./check_page.py --text mypage.txt                # check text copied from the web editor
  ./check_page.py --project 123456 --no-links      # skip the link check

The first run prints which fields the API returned, because the docs don't
list them; the checker looks at every text-like field it finds.
Project-specific words for the spell check go in dictionary.txt next to this
file, one per line.

Standard library only. Python 3.9+.
"""

from __future__ import annotations

import argparse
import difflib
import html
import json
import os
import re
import shutil
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What actually works (found by trying it, the docs are wrong on both counts):
# the developer platform's /v2 is served from dev.hackaday.io, not hackaday.io
# (which is the website and answers HTML), and it reads the key from ?api_key=,
# not the X-API-Key header the docs describe. The older /v1 on api.hackaday.io
# also takes ?api_key=. Try both.
BASES = [("https://dev.hackaday.io/v2", "query"), ("https://api.hackaday.io/v1", "query")]
DEFAULT_BASE = None
DEFAULT_DRAFT = HERE / "hackaday-page.md"
DICTIONARY = HERE / "dictionary.txt"
KEY_FILE = Path.home() / ".config" / "hackaday" / "api_key"
SUMMARY_LIMIT = 140
UA = "analog-redial-page-checker/1 (read-only; github.com/sdylewski/Analog-Redial)"


# ------------------------------------------------------------------ fetching

def load_key() -> str:
    key = os.environ.get("HACKADAY_API_KEY", "").strip()
    if key:
        return key
    if KEY_FILE.is_file():
        mode = KEY_FILE.stat().st_mode & 0o077
        if mode:
            print(f"note: {KEY_FILE} is readable by others; run: chmod 600 {KEY_FILE}",
                  file=sys.stderr)
        return KEY_FILE.read_text().strip()
    sys.exit("No API key. Set HACKADAY_API_KEY or put the key in "
             f"{KEY_FILE} (see the setup notes at the top of this script).")


def project_id(arg: str) -> str:
    m = re.search(r"(?:project/)?(\d+)", arg)
    if not m:
        sys.exit(f"Can't find a project number in '{arg}'.")
    return m.group(1)


class ApiError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code, self.message = code, message


def get(base: str, auth: str, path: str, key: str, params: dict | None = None):
    params = dict(params or {})
    headers = {"Accept": "application/json", "User-Agent": UA}
    if auth == "header":
        headers["X-API-Key"] = key
    else:
        params["api_key"] = key
    url = base.rstrip("/") + path + ("?" + urllib.parse.urlencode(params) if params else "")
    req = urllib.request.Request(url, headers=headers)
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                body = r.read()
        except urllib.error.HTTPError as e:
            if e.code == 429 and attempt < 2:
                time.sleep(5 * (attempt + 1))
                continue
            msg = ""
            try:
                msg = json.loads(e.read().decode("utf-8", "replace")).get("message", "")
            except Exception:
                pass
            raise ApiError(e.code, (msg or f"HTTP {e.code}").replace(key, "<key>"))
        except (urllib.error.URLError, TimeoutError, ssl.SSLError) as e:
            sys.exit(f"{path}: couldn't reach the server ({e})")
        try:
            return json.loads(body)
        except ValueError:
            raise ApiError(404, "the reply wasn't JSON (wrong address?)")
    sys.exit(f"{path}: gave up")


def list_of_dicts(js):
    """The list of records in a reply, whatever the wrapper is called."""
    if isinstance(js, list):
        return [x for x in js if isinstance(x, dict)]
    if isinstance(js, dict):
        for v in js.values():
            if isinstance(v, list) and v and isinstance(v[0], dict):
                return v
    return []


def optional(base, auth, path, key, params=None):
    """A section that may not exist on this API version."""
    try:
        return get(base, auth, path, key, params)
    except ApiError:
        return None


def fetch_all(base: str | None, auth: str | None, pid: str, key: str) -> dict:
    combos = [(base, auth or "header")] if base else BASES
    project, used, errors = None, None, []
    for b, a in combos:
        try:
            project = get(b, a, f"/projects/{pid}", key)
            used = (b, a)
            break
        except ApiError as e:
            errors.append((b, e))
    if project is None:
        for b, e in errors:
            print(f"  {b}: {e.message}", file=sys.stderr)
        # The first address is the one this key is for; the legacy /v1 one always
        # complains about a v2 key, so only the first answer decides what to tell you.
        first = errors[0][1]
        wrong_key = first.code in (401, 403) or "key" in first.message.lower()
        not_found = first.code == 404 or "not found" in first.message.lower()
        if wrong_key:
            sys.exit("Couldn't read the project.\nHackaday says the API key is wrong or missing. Use the "
                     "value labelled \"API Key\" on your app's page at dev.hackaday.io, not the client secret.")
        if not_found:
            sys.exit("Couldn't read the project: the API says it doesn't exist.\n"
                     "If the number is right, the project is probably still an unpublished draft. A key alone "
                     "only sees published projects (drafts need OAuth with the private-apis:read scope).\n"
                     "Until it's published, copy the page text into a file and run:  ./check_page.py --text FILE")
        sys.exit("Couldn't read the project. Check the project number.")
    base, auth = used
    print(f"Connected: {base}")
    logs, page = [], 1
    while page <= 50:
        js = optional(base, auth, f"/projects/{pid}/logs", key, {"page": page, "per_page": 50, "limit": 50})
        batch = list_of_dicts(js) if js else []
        logs += batch
        if len(batch) < 50:
            break
        page += 1
    out = {"project": project, "logs": logs}
    for extra in ("details", "instructions"):      # sections the older API serves separately
        js = optional(base, auth, f"/projects/{pid}/{extra}", key)
        if js:
            out[extra] = js
    return out


# ---------------------------------------------------------------- extracting

class _Text(HTMLParser):
    def __init__(self):
        super().__init__()
        self.out: list[str] = []

    def handle_data(self, d):
        self.out.append(d)

    def handle_starttag(self, tag, attrs):
        if tag in ("p", "br", "li", "h1", "h2", "h3", "h4", "div", "tr"):
            self.out.append("\n\n" if tag in ("p", "h1", "h2", "h3", "h4", "div") else "\n")


def to_text(s: str) -> str:
    if "<" in s and re.search(r"</?[a-zA-Z]+[^>]*>", s):
        p = _Text()
        p.feed(s)
        s = "".join(p.out)
    s = html.unescape(s)
    return re.sub(r"[ \t]+\n", "\n", re.sub(r"\n{3,}", "\n\n", s)).strip()


def walk(obj, path=""):
    """Every (path, string) in a JSON value."""
    if isinstance(obj, str):
        yield path, obj
    elif isinstance(obj, dict):
        for k, v in obj.items():
            yield from walk(v, f"{path}.{k}" if path else k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from walk(v, f"{path}[{i}]")


TEXTY = re.compile(r"(summary|description|details|content|body|text|title|name|tagline|message)", re.I)
SKIP = re.compile(r"(url|image|avatar|slug|link|id$|hash|created|updated|date|time)", re.I)


RAW: dict[str, str] = {}     # field path -> the text as the API sent it (for link addresses)


def text_fields(data: dict) -> list[tuple[str, str]]:
    found = []
    RAW.clear()
    for path, val in walk(data):
        leaf = path.split(".")[-1]
        if SKIP.search(leaf) and not TEXTY.search(leaf):
            continue
        if TEXTY.search(leaf) or len(val) > 80:
            t = to_text(val)
            if len(t) >= 3:
                found.append((path, t))
                RAW[path] = val
    return found


# -------------------------------------------------------------------- checks

URL = re.compile(r"https?://[^\s<>\"')\]]+")
Issue = tuple[str, str, str]   # (where, kind, detail)


def snippet(text: str, pos: int, n: int = 40) -> str:
    return text[max(0, pos - n): pos + n].replace("\n", " ")


def check_text(where: str, text: str, is_summary: bool) -> list[Issue]:
    out: list[Issue] = []
    if is_summary and len(text) > SUMMARY_LIMIT:
        out.append((where, "summary too long", f"{len(text)} characters, the limit is {SUMMARY_LIMIT}"))
    for m in re.finditer(r"\b([A-Za-z']{2,})\s+\1\b", text, re.I):
        out.append((where, "repeated word", f"'{m.group(0)}': ...{snippet(text, m.start())}..."))
    for m in re.finditer(r"[a-z]{2,}[.!?][A-Z][a-z]", text):
        if not re.search(r"\.(js|py|io|com|md|org|net)\b", text[m.start(): m.end() + 4]):
            out.append((where, "no space after full stop", f"...{snippet(text, m.start())}..."))
    for m in re.finditer(r"(?<=\S)  +(?=\S)", text):
        out.append((where, "double space", f"...{snippet(text, m.start())}..."))
    for m in re.finditer(r"\[YOU[^\]]*\]|\bTODO\b|\bXXX\b|\bFIXME\b|lorem ipsum", text, re.I):
        out.append((where, "leftover note", m.group(0)[:60]))
    return out


def norm(p: str) -> str:
    return re.sub(r"\W+", " ", p.lower()).strip()


def duplicates(fields: list[tuple[str, str]]) -> list[Issue]:
    seen: dict[str, list[str]] = {}
    for where, text in fields:
        for para in re.split(r"\n\s*\n", text):
            n = norm(para)
            if len(n) >= 60:
                seen.setdefault(n, []).append(where)
    return [(", ".join(sorted(set(w))), "paragraph appears more than once",
             f"x{len(w)}: {n[:70]}...") for n, w in seen.items() if len(w) > 1]


def check_links(fields: list[tuple[str, str]]) -> list[Issue]:
    urls: dict[str, str] = {}
    for where, text in fields:
        # addresses live in href="..." attributes that to_text() strips, so read the raw text too
        for u in URL.findall(RAW.get(where, "") + " " + text):
            urls.setdefault(html.unescape(u).rstrip(".,;:'\""), where)
    out: list[Issue] = []
    ctx = ssl.create_default_context()
    for u, where in sorted(urls.items()):
        status = None
        for method in ("HEAD", "GET"):
            try:
                req = urllib.request.Request(u, method=method, headers={"User-Agent": UA})
                with urllib.request.urlopen(req, timeout=12, context=ctx) as r:
                    status = r.status
                break
            except urllib.error.HTTPError as e:
                status = e.code
                if method == "HEAD" and e.code in (403, 405, 501):
                    continue   # some servers refuse HEAD
                break
            except (urllib.error.URLError, TimeoutError, ssl.SSLError, ValueError) as e:
                status = f"{getattr(e, 'reason', e)}"
                break
        if not isinstance(status, int) or status >= 400:
            out.append((where, "link doesn't load", f"{u}  ({status})"))
        time.sleep(0.3)
    return out


def words_ok() -> set[str]:
    if DICTIONARY.is_file():
        return {w.strip().lower() for w in DICTIONARY.read_text().splitlines() if w.strip()}
    return set()


def check_spelling(fields: list[tuple[str, str]]) -> tuple[list[Issue], str]:
    tool = shutil.which("aspell") or shutil.which("hunspell")
    if not tool:
        return [], "spell check skipped: install aspell (brew install aspell)"
    cmd = [tool, "list"] if tool.endswith("aspell") else [tool, "-l"]
    allow, out = words_ok(), []
    for where, text in fields:
        text = URL.sub(" ", text)
        text = re.sub(r"`[^`]*`", " ", text)
        r = subprocess.run(cmd, input=text, capture_output=True, text=True)
        for w in sorted(set(r.stdout.split())):
            if w.lower() not in allow and not re.search(r"\d", w) and not w.isupper():
                out.append((where, "possible misspelling", w))
    return out, ""


def check_draft(fields: list[tuple[str, str]], draft_path: Path) -> list[Issue]:
    if not draft_path.is_file():
        return []
    md = draft_path.read_text()
    a = md.find("## 4. Details")
    b = md.find("\n---\n", a + 1) if a >= 0 else -1
    if a < 0 or b < 0:
        return []
    own_notes = ("[YOU", "This is the text as it stands", "*(The live page also has")
    draft = [norm(p) for p in re.split(r"\n\s*\n", md[a:b])
             if len(norm(p)) >= 60 and not any(n in p for n in own_notes)]
    # the details field is the longest text field
    live_field = max(fields, key=lambda f: len(f[1]), default=None)
    if not live_field:
        return []
    live = [norm(p) for p in re.split(r"\n\s*\n", live_field[1]) if len(norm(p)) >= 60]

    def best(p: str, pool: list[str]) -> tuple[float, str]:
        scored = [(difflib.SequenceMatcher(None, p, q).ratio(), q) for q in pool]
        return max(scored, default=(0.0, ""))

    def word_diff(old: str, new: str) -> str:
        sm = difflib.SequenceMatcher(None, old.split(), new.split())
        parts = []
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op != "equal":
                parts.append(f"'{' '.join(old.split()[i1:i2])}' -> '{' '.join(new.split()[j1:j2])}'")
        return "; ".join(parts[:6])

    out: list[Issue] = []
    for p in live:
        ratio, q = best(p, draft)
        if ratio == 1.0:
            continue
        if ratio >= 0.6:     # the same paragraph with something changed: show what
            out.append((live_field[0], "page differs from the draft (draft -> page)", word_diff(q, p)))
        else:
            out.append((live_field[0], "on the page but not in the draft", p[:80]))
    for q in draft:
        if best(q, live)[0] < 0.6:
            out.append(("draft", "in the draft but not on the page", q[:80]))
    return out


# ---------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description="Read-only check of a hackaday.io project page.")
    ap.add_argument("--project", help="project number or URL")
    ap.add_argument("--offline", metavar="FILE", help="check a saved reply instead of fetching")
    ap.add_argument("--text", metavar="FILE", help="check a plain text or Markdown file (e.g. the page text "
                                                   "copied out of the web editor); no API key needed")
    ap.add_argument("--dump", metavar="FILE", help="save the raw reply (it holds no key)")
    ap.add_argument("--base-url", default=DEFAULT_BASE, help=argparse.SUPPRESS)
    ap.add_argument("--auth", choices=("header", "query"), help=argparse.SUPPRESS)
    ap.add_argument("--draft", type=Path, default=DEFAULT_DRAFT)
    ap.add_argument("--no-links", action="store_true")
    ap.add_argument("--no-spell", action="store_true")
    args = ap.parse_args()

    if args.text:
        data = {"project": {"description": Path(args.text).read_text()}, "logs": []}
    elif args.offline:
        data = json.loads(Path(args.offline).read_text())
    else:
        pid = project_id(args.project or os.environ.get("HACKADAY_PROJECT_ID", ""))
        data = fetch_all(args.base_url, args.auth, pid, load_key())
        if args.dump:
            Path(args.dump).write_text(json.dumps(data, indent=2))
            print(f"saved the reply to {args.dump}")

    fields = text_fields(data)
    print(f"Fetched the project and {len(data.get('logs', []))} log(s). Text fields found:")
    for path, t in fields:
        print(f"  {path:<40} {len(t):>6} chars")
    if not fields:
        sys.exit("No text fields found: check the project number, or send --dump output to look at.")

    issues: list[Issue] = []
    for where, text in fields:
        issues += check_text(where, text, is_summary=bool(re.search(r"summary|tagline", where, re.I)))
    issues += duplicates(fields)
    if not args.no_links:
        print("Checking links...")
        issues += check_links(fields)
    note = ""
    if not args.no_spell:
        sp, note = check_spelling(fields)
        issues += sp
    issues += check_draft(fields, args.draft)

    print()
    if not issues:
        print("No problems found." + (f" ({note})" if note else ""))
        return 0
    last = None
    for where, kind, detail in sorted(issues, key=lambda i: (i[1], i[0])):
        if kind != last:
            print(f"\n== {kind}")
            last = kind
        print(f"  [{where}] {detail}")
    print(f"\n{len(issues)} item(s) to look at." + (f" ({note})" if note else ""))
    return 1


if __name__ == "__main__":
    sys.exit(main())
