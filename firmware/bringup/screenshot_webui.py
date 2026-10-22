#!/usr/bin/env python3
"""Capture full-page screenshots of the board's web UI into docs/pics/.

Each page is saved as <page-name>.png (status.png, recordings.png,
settings-<tab>.png) in the dark theme, which is what the docs show, and as
<page-name>-light.png in the light theme. A 400x250 thumbnail of each goes in
docs/pics/thumbs/ (macOS `sips`). The Wi-Fi name, the board's name and the
serial number are replaced with generic ones. Re-running overwrites the previous set.

How: curl fetches each page and its stylesheet into a temp folder, and
headless Chrome renders that local copy. Chrome is never pointed at the
board itself -- on macOS a browser launched from a terminal has no "Local
Network" permission and just hangs on a LAN address, while curl works.
Pages are rendered server-side, so the local copy looks identical.

The board's setup password appears in plain text in the Dial Codes page's
gen_prompt.py commands; it is replaced with a placeholder before rendering.

Usage:
    firmware/bringup/screenshot_webui.py 192.168.0.214
    firmware/bringup/screenshot_webui.py phone.local --only status settings-general
"""
import argparse
import html as htmllib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
WIDTH = 1280

PAGES = {
    "status": "/",
    "recordings": "/recordings",
    "settings-general": "/settings?tab=general",
    "settings-phone": "/settings?tab=phone",
    "settings-dialcodes": "/settings?tab=dialcodes",
    "settings-voip": "/settings?tab=voip",
    "settings-bluetooth": "/settings?tab=bluetooth",
    "settings-home": "/settings?tab=home",
    "settings-hardware": "/settings?tab=hardware",
    "settings-storage": "/settings?tab=storage",
    "settings-diag": "/settings?tab=diag",
}

STATIC_FILES = ["static/style.css", "static/favicon.svg"]

HEIGHT_PROBE = ("<script>window.addEventListener('load',function(){"
                "document.title='H='+document.documentElement.scrollHeight;});</script>")


THEMES = {"dark": "", "light": "-light"}


def curl(url: str) -> bytes:
    r = subprocess.run(["curl", "-sf", "-m", "20", url], capture_output=True)
    if r.returncode != 0:
        sys.exit(f"curl failed for {url} (exit {r.returncode})")
    return r.stdout


CHROME_FLAGS = ["--headless=new", "--disable-gpu", "--hide-scrollbars",
                "--no-first-run", "--no-default-browser-check"]


def chrome_dump_dom(page: str, profile: str) -> str:
    # Like the screenshot below: Chrome may never exit after printing, so
    # read until the document is complete and then stop it.
    out_path = page + ".dom"
    with open(out_path, "w") as out:
        proc = subprocess.Popen([CHROME, *CHROME_FLAGS, f"--user-data-dir={profile}",
                                 f"--window-size={WIDTH},900", "--dump-dom", f"file://{page}"],
                                stdout=out, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 60
        while time.time() < deadline:
            time.sleep(0.3)
            with open(out_path) as fh:
                dom = fh.read()
            if "</html>" in dom or proc.poll() is not None:
                return dom
        sys.exit(f"timed out measuring {page}")
    finally:
        if proc.poll() is None:
            proc.kill()


def chrome_screenshot(page: str, png: str, height: int, profile: str) -> None:
    # Chrome writes the PNG and then often never exits (its updater lingers),
    # so wait for the file rather than for the process.
    if os.path.exists(png):
        os.remove(png)
    proc = subprocess.Popen([CHROME, *CHROME_FLAGS, f"--user-data-dir={profile}",
                             f"--window-size={WIDTH},{height}", f"--screenshot={png}",
                             f"file://{page}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 60
        last = -1
        while time.time() < deadline:
            time.sleep(0.5)
            size = os.path.getsize(png) if os.path.exists(png) else -1
            if size > 0 and size == last:
                return
            if proc.poll() is not None and size > 0:
                return
            last = size
        sys.exit(f"timed out capturing {png}")
    finally:
        if proc.poll() is None:
            proc.kill()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="board IP or hostname")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "..", "docs", "pics"))
    ap.add_argument("--themes", nargs="+", choices=THEMES.keys(), default=list(THEMES),
                    help="which themes to capture (default: both)")
    ap.add_argument("--only", nargs="+", choices=PAGES.keys(), help="capture just these pages")
    args = ap.parse_args()

    if not os.path.exists(CHROME):
        sys.exit(f"Google Chrome not found at {CHROME}")
    base = f"http://{args.host}"
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    work = tempfile.mkdtemp(prefix="webui-shots-")
    profile = os.path.join(work, "chrome-profile")
    try:
        os.makedirs(os.path.join(work, "static"))
        for f in STATIC_FILES:
            with open(os.path.join(work, f), "wb") as fh:
                fh.write(curl(f"{base}/{f}"))

        # Network names shouldn't end up in published docs.
        status = json.loads(curl(f"{base}/api/status"))
        ssids = [htmllib.escape(s) for s in [status.get("wifi_ssid", "")] if s]

        # Neither should the board's own name; the docs show the default one.
        board_name = status.get("board_name", "")
        serial = status.get("serial", "")

        for name in (args.only or PAGES):
            html = curl(base + PAGES[name]).decode("utf-8", "replace")
            html = re.sub(r'(href|src)="/static/', r'\1="static/', html)
            # Page scripts poll the board / auto-refresh, which keeps headless
            # Chrome from ever finishing -- the pages render server-side anyway.
            html = re.sub(r"<script\b.*?</script>", "", html, flags=re.S | re.I)
            html = re.sub(r'<meta http-equiv="refresh"[^>]*>', "", html, flags=re.I)
            html = re.sub(r'(--secret &quot;)[^&]*(&quot;)', r'\1your-setup-password\2', html)
            html = re.sub(r'(id="setup_pw_input" name="password" value=")[^"]*"',
                          r'\1your-setup-password"', html)
            if board_name and board_name != "phone":
                html = re.sub(re.escape(board_name), "phone", html, flags=re.I)
            if len(serial) >= 12:
                html = html.replace(serial, serial[:6] + "XXXXXX")
            for ssid in ssids:
                html = html.replace(ssid, "MyHomeWiFi")
            html = re.sub(r"(<tr><td>)[^<]*?( \u25cf)?(</td><td>(?:\U0001f512|\u2014)</td>)",
                          r"\1MyHomeWiFi\2\3", html)
            for theme in args.themes:
                themed = re.sub(r"<html\b", f'<html data-theme="{theme}"', html, count=1)
                page = os.path.join(work, f"{name}{THEMES[theme]}.html")
                with open(page, "w") as fh:
                    fh.write(themed.replace("</body>", HEIGHT_PROBE + "</body>", 1))

                dom = chrome_dump_dom(page, profile)
                m = re.search(r"<title>H=(\d+)</title>", dom)
                height = int(m.group(1)) if m else 900

                png = os.path.join(out_dir, f"{name}{THEMES[theme]}.png")
                chrome_screenshot(page, png, height, profile)
                # Thumbnail: the top 1280x800 of the page, scaled to 400 px wide.
                thumb = os.path.join(out_dir, "thumbs", f"{name}{THEMES[theme]}.png")
                os.makedirs(os.path.dirname(thumb), exist_ok=True)
                chrome_screenshot(page, thumb, 800, profile)
                subprocess.run(["sips", "--resampleWidth", "400", thumb], check=True, capture_output=True)
                print(f"{name}{THEMES[theme]}.png  {WIDTH}x{height}")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
