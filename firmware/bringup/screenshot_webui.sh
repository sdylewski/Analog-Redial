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
#   board-host defaults to dualphone2.local -- if captures come back stuck
#   mid-load (a blank page with the progress bar still showing, even after
#   raising LOAD_DELAY a lot), that's not a slow page: Safari's own .local
#   /mDNS resolution has been observed to hang for 15s+ on this exact kind
#   of hostname while curl resolves the same name instantly through a
#   different path. Pass the board's raw IP instead and it loads normally.
#   output-dir defaults to docs/pics (relative to this script's own
#   location, i.e. always the repo's docs/pics regardless of cwd).
#
# Re-run any time the web UI changes -- filenames are fixed per page, so
# this overwrites in place rather than piling up dated duplicates.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

HOST="${1:-dualphone2.local}"
OUTDIR="${2:-${SCRIPT_DIR}/../../docs/pics}"
LOAD_DELAY="${LOAD_DELAY:-2.5}"     # seconds to wait after navigation before capturing
WIN_BOUNDS="100, 100, 1380, 1000"   # x0,y0,x1,y1 -- ~1280x900 content area

# Pixels to crop off the TOP of each raw capture, removing Safari's own
# chrome (traffic lights, URL bar, tab bar) so only the page content
# remains. Measured at 220px on a window with an existing multi-tab bar
# visible (the common case -- "make new document" below tends to add a
# tab to whatever Safari window is already open rather than opening a
# genuinely bare one, since that's controlled by the user's own Safari
# tab-handling preference, not this script). If your crop comes out
# short (a sliver of chrome left) or long (clipping into the page's own
# header), override: CHROME_CROP_PX=180 firmware/bringup/screenshot_webui.sh ...
# Set to 0 to disable cropping entirely and keep the raw capture.
CHROME_CROP_PX="${CHROME_CROP_PX:-220}"

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

# Crops a raw capture's chrome off the top in place. Plain `sips -c H W`
# (no --cropOffset) anchors the crop at the BOTTOM, discarding rows from
# the top -- exactly what's wanted here. --cropOffset looks like the more
# direct way to say "crop N px off the top," but combining it with -c
# silently no-ops on this macOS version (confirmed: crop dimensions came
# back unchanged from the original every time), so this deliberately
# doesn't use it.
crop_chrome() {
  local file="$1"
  [ "$CHROME_CROP_PX" -le 0 ] && return 0
  local w h new_h
  w=$(sips -g pixelWidth "$file" | awk '/pixelWidth/{print $2}')
  h=$(sips -g pixelHeight "$file" | awk '/pixelHeight/{print $2}')
  new_h=$((h - CHROME_CROP_PX))
  if [ "$new_h" -le 0 ]; then
    echo "    warning: CHROME_CROP_PX (${CHROME_CROP_PX}) >= image height (${h}), skipping crop"
    return 0
  fi
  sips -c "$new_h" "$w" "$file" >/dev/null
}

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
  crop_chrome "$out"
  echo "    saved ${out}"
done

osascript -e 'tell application "Safari" to close window 1' >/dev/null 2>&1 || true
echo "Done -- screenshots in ${OUTDIR}/"
