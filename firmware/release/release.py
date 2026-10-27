#!/usr/bin/env python3
"""Build the firmware for every board type and publish it for the web UI's
"Check for updates".

The board's web page (Settings -> Hardware -> Firmware) reads a small manifest,
latest.json, from the repo's `firmware` branch through raw.githubusercontent.com
(which allows a browser on another origin to read it; GitHub's own release
downloads don't), compares versions, and on "Install" downloads the right image
and posts it to /ota_upload -- the same upload as picking the .bin by hand, so
it has the same checks: a valid image, built for this board, and automatic
rollback if the new one doesn't start. See components/webui (FW_MANIFEST_URL).

  # 1. tag the commit you want to release (annotated; the version in the image
  #    comes from `git describe --match "v*"`), tree clean
  git tag -a v1.0.0 -m "v1.0.0"
  # 2. build everything, write release/out/v1.0.0/ (no network, nothing pushed)
  ./release/release.py --version v1.0.0 --notes "What changed, one or two lines"
  # 3. publish: force-pushes an orphan `firmware` branch holding only this
  #    release (images + latest.json), so the repo doesn't grow by ~6 MB a release
  ./release/release.py --version v1.0.0 --publish --no-build

Run it from an ESP-IDF shell (`. ~/esp/esp-idf/export.sh`). Each board builds in
its own directory under build.release/ with an explicit SDKCONFIG, so the
tracked sdkconfig is never touched.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent
REPO = FIRMWARE.parent
OUT = HERE / "out"

REPO_SLUG = "sdylewski/Analog-Redial"
RAW_BASE = f"https://raw.githubusercontent.com/{REPO_SLUG}/firmware/"

# image id (what the firmware reports as BOARD_IMAGE_ID) -> sdkconfig overlay
OVERLAYS = {
    "smd2": "sdkconfig.smd2.defaults",
    "smd4": "sdkconfig.smd4.defaults",
    "smd5": "sdkconfig.smd5.defaults",
    "smd5_cheap": "sdkconfig.smd5_cheap.defaults",
}
DEFAULT_IMAGES = ["smd4/16MB", "smd5/16MB", "smd5_cheap/16MB", "smd2/16MB",
                  "smd4/8MB", "smd5/8MB", "smd5_cheap/8MB", "smd2/8MB"]


def run(cmd: list[str], cwd: Path = FIRMWARE, check: bool = True) -> subprocess.CompletedProcess:
    print("  $ " + " ".join(cmd))
    return subprocess.run(cmd, cwd=cwd, check=check)


def git(*args: str, cwd: Path = REPO) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def check_tree(version: str) -> None:
    if not re.fullmatch(r"v\d+\.\d+\.\d+", version):
        sys.exit(f"--version must look like v1.2.3, not {version!r}")
    if git("status", "--porcelain", "--untracked-files=no"):
        sys.exit("The working tree has uncommitted changes; the image would say '-dirty'. Commit first.")
    described = git("describe", "--always", "--dirty", "--match", "v*")
    if described != version:
        sys.exit(f"HEAD describes as {described!r}, not {version!r}. Tag this commit first:\n"
                 f"  git tag -a {version} -m {version}")


def build_one(image: str, version: str) -> Path:
    board, flash = image.split("/")
    if board not in OVERLAYS or flash not in ("16MB", "8MB"):
        sys.exit(f"unknown image {image!r} (want e.g. smd5/16MB)")
    bdir = f"build.release/{board}_{flash}"
    chain = ["sdkconfig.defaults", OVERLAYS[board]] + (["sdkconfig.8mb.defaults"] if flash == "8MB" else [])
    common = ["idf.py", "-B", bdir, "-D", f"SDKCONFIG={bdir}/sdkconfig",
              "-D", "SDKCONFIG_DEFAULTS=" + ";".join(chain)]
    if not (FIRMWARE / bdir / "sdkconfig").exists():
        run(common + ["set-target", "esp32c6"])
    # reconfigure so version.txt (git describe) is regenerated for this tag
    run(common + ["reconfigure"])
    run(common + ["build"])
    binf = FIRMWARE / bdir / "rotary_phone_gateway.bin"
    data = binf.read_bytes()
    if version.encode() not in data or (version + "-dirty").encode() in data:
        sys.exit(f"{binf} doesn't carry the clean version string {version!r}")
    marker = b"\x01AR-BOARD\x02" + image.encode() + b"\x03"
    if marker not in data:
        sys.exit(f"{binf} isn't marked as an image for {image}")
    return binf


def write_release(version: str, images: dict[str, Path], notes: str) -> Path:
    out = OUT / version
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    manifest_images = {}
    for image, src in images.items():
        name = image.replace("/", "-") + ".bin"
        dest_rel = f"{version}/{name}"
        (out / name).write_bytes(src.read_bytes())
        data = src.read_bytes()
        manifest_images[image] = {"file": dest_rel, "size": len(data),
                                  "sha256": hashlib.sha256(data).hexdigest()}
    manifest = {
        "version": version,
        "released": datetime.date.today().isoformat(),
        "notes": notes,
        "base": RAW_BASE,
        "images": manifest_images,
    }
    (out / "latest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\nwrote {out}/ :")
    for p in sorted(out.iterdir()):
        print(f"  {p.name}  {p.stat().st_size:,} bytes")
    return out


def publish(version: str) -> None:
    out = OUT / version
    if not (out / "latest.json").exists():
        sys.exit(f"{out}/latest.json not found: build first (drop --no-build)")
    manifest = json.loads((out / "latest.json").read_text())
    with tempfile.TemporaryDirectory() as td:
        wt = Path(td) / "wt"
        git("worktree", "add", "--detach", str(wt), "HEAD")
        try:
            git("checkout", "--orphan", "firmware-publish", cwd=wt)
            git("rm", "-rf", "--quiet", ".", cwd=wt)
            (wt / version).mkdir()
            for p in out.glob("*.bin"):
                shutil.copy2(p, wt / version / p.name)
            shutil.copy2(out / "latest.json", wt / "latest.json")
            (wt / "README.md").write_text(
                "# firmware branch\n\nGenerated by `firmware/release/release.py`: the images and "
                "`latest.json` that a board's web page reads for \"Check for updates\". Force-pushed on "
                "every release; it only ever holds the current one. Don't edit by hand.\n\n"
                f"Current release: **{manifest['version']}** ({manifest['released']}).\n")
            git("add", "-A", cwd=wt)
            git("commit", "-q", "-m", f"Firmware {version}", cwd=wt)
            print(f"force-pushing the `firmware` branch ({version}) to origin ...")
            run(["git", "push", "--force", "origin", "firmware-publish:refs/heads/firmware"], cwd=wt)
        finally:
            git("worktree", "remove", "--force", str(wt))
            subprocess.run(["git", "branch", "-D", "firmware-publish"], cwd=REPO, capture_output=True)
    print(f"\npublished. latest.json: {RAW_BASE}latest.json")
    print("(a browser can only read it once the repository is public)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="vX.Y.Z; HEAD must be tagged with it")
    ap.add_argument("--images", default=",".join(DEFAULT_IMAGES),
                    help="comma-separated image ids (default: %(default)s)")
    ap.add_argument("--notes", default="", help="shown under the Install button")
    ap.add_argument("--no-build", action="store_true", help="reuse release/out/<version>/ (with --publish)")
    ap.add_argument("--publish", action="store_true", help="force-push the firmware branch")
    args = ap.parse_args()

    if not args.no_build:
        if not shutil.which("idf.py"):
            ap.error("idf.py not found -- run `. ~/esp/esp-idf/export.sh` first")
        check_tree(args.version)
        built = {img: build_one(img, args.version) for img in args.images.split(",")}
        write_release(args.version, built, args.notes)
    if args.publish:
        publish(args.version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
