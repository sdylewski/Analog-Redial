#!/usr/bin/env bash
# screenshot_webui.sh -- capture screenshots of the board's web UI for
# docs/README/Hackaday.io use.
#
# macOS-only, unlike the other bringup/ tools (Python, cross-platform):
# drives a real Safari window via AppleScript and captures it with the
# screencapture CLI, both because a headless browser can't reach the
# board's LAN IP (macOS's per-app "Local Network" permission blocks it,
# while Safari already has that permission from ordinary browsing) and
# because there's no way to render this any other way without a real
# browser engine. screencapture also needs macOS's "Screen Recording"
# permission granted to whatever terminal app runs this script --
# System Settings -> Privacy & Security -> Screen Recording -- and a
# restart of that terminal app after granting it, or it fails with
# "could not create image from window".
#
# Usage: firmware/bringup/screenshot_webui.sh [board-host] [output-dir]
#   board-host defaults to dualphone2.local
#   output-dir defaults to ./screenshots
#
# Re-run any time the web UI changes -- filenames are fixed per page, so
# this overwrites in place rather than piling up dated duplicates.

set -euo pipefail

HOST="${1:-dualphone2.local}"
OUTDIR="${2:-./screenshots}"
LOAD_DELAY="${LOAD_DELAY:-2.5}"     # seconds to wait after navigation before capturing
WIN_BOUNDS="100, 100, 1380, 1000"   # x0,y0,x1,y1 -- ~1280x900 content area

mkdir -p "$OUTDIR"

# name:path pairs -- one screenshot per page. Add more here as the UI
# grows; keep names filesystem-safe (no spaces/slashes).
PAGES=(
  "status:/"
  "settings-general:/settings?tab=general"
  "settings-voip:/settings?tab=voip"
  "settings-dialcodes:/settings?tab=dialcodes"
  "settings-hardware:/settings?tab=hardware"
  "recordings:/recordings"
)

echo "==> Opening Safari window against http://${HOST}/ ..."
osascript <<EOF
tell application "Safari"
    activate
    make new document with properties {URL:"http://${HOST}/"}
    set bounds of window 1 to {${WIN_BOUNDS}}
end tell
EOF
sleep "$LOAD_DELAY"

WINID=$(osascript -e 'tell application "Safari" to id of window 1')

for entry in "${PAGES[@]}"; do
  name="${entry%%:*}"
  path="${entry#*:}"
  url="http://${HOST}${path}"
  out="${OUTDIR}/${name}.png"

  echo "==> ${name}  (${url})"
  osascript -e "tell application \"Safari\" to set URL of document 1 to \"${url}\""
  sleep "$LOAD_DELAY"
  if ! screencapture -o -l"$WINID" "$out" 2>/tmp/screenshot_webui_err; then
    echo "    FAILED -- $(cat /tmp/screenshot_webui_err)"
    echo "    (likely missing Screen Recording permission -- see this script's header comment)"
    exit 1
  fi
  echo "    saved ${out}"
done

osascript -e 'tell application "Safari" to close window 1' >/dev/null 2>&1 || true
echo "Done -- screenshots in ${OUTDIR}/"
