#!/usr/bin/env python3
"""Bring-up: BK3266 Bluetooth module (the BK3266 daughter board on J2).

Steps, each stopping at the first thing that needs fixing:
  1. protocol code sane      (bk_selftest -- needs no module)
  2. module answers          (bk_probe: boot chatter + version query)
  3. link driver comes up    (bt_enable 1, then bk_state)
  4. pairing                 (you pair a phone; reads the connection back)
  5. incoming call           (you call the paired phone; checks IC + caller ID)
  6. call ends               (you hang up; checks the state returns to idle --
                              the one thing the protocol table doesn't say)

Needs: firmware built for the board, Settings -> Hardware set to the BK3266
module and rebooted (steps 3+). Record what you see in
archive/hardware/bk3266.md -- that file is where the "unverified" list lives.

  ./test_bk3266.py                 # all steps
  ./test_bk3266.py --only probe    # one step
"""

from __future__ import annotations
import argparse
import re
import sys
import time

from phone_serial import PhoneConsole, add_common_args, pick_port

STEPS = ["selftest", "probe", "link", "pairing", "incoming", "hangup"]


def state(console: PhoneConsole) -> dict:
    """bk_state output as a dict of lowercase label -> value."""
    out = {}
    for line in console.send("bk_state", timeout_s=3):
        m = re.match(r"^\s*([a-z ]+?)\s*:\s*(.*)$", line)
        if m:
            out[m.group(1).strip()] = m.group(2).strip().strip("'")
    return out


def wait_state(console, key, want, seconds):
    """Polls bk_state until out[key] satisfies want() or time runs out."""
    end = time.time() + seconds
    last = {}
    while time.time() < end:
        last = state(console)
        if want(last.get(key, "")):
            return True, last
        time.sleep(1.0)
    return False, last


def step_selftest(console) -> bool:
    lines = console.send("bk_selftest", timeout_s=5)
    print("\n".join("  " + l for l in lines))
    return any("PASS" in l for l in lines)


def step_probe(console) -> bool:
    console.send("bt_enable 0", timeout_s=3)       # the probe owns the UART
    print("Power-cycle the module now if you can (unplug/replug the board) so its boot\n"
          "message is caught; otherwise this only sees the reply to the version query.")
    input("Press Enter to probe... ")
    lines = console.send("bk_probe", timeout_s=12)
    print("\n".join("  " + l for l in lines))
    text = "\n".join(lines)
    if "frame:" in text:
        return True
    print("\nNo decoded frame. Read the 'result:' line above: 0 bytes = power/wiring/baud;\n"
          "printable bytes = the text AT firmware (try bk_at BT+VER).")
    return False


def step_link(console) -> bool:
    console.send("bt_enable 1", timeout_s=3)
    time.sleep(6)                                   # boot settle + startup queries
    st = state(console)
    for k in ("task", "last frame", "booted", "own address", "version", "own name"):
        print(f"  {k:12}: {st.get(k, '?')}")
    return st.get("task") == "running" and "none yet" not in st.get("last frame", "none yet")


def step_pairing(console) -> bool:
    print("On your phone: Bluetooth settings, forget this board if listed earlier.")
    console.send("bk_cmd 01", timeout_s=3)         # BT_CMD_MATCH: enter pairing
    print("Pairing mode requested. Pair the phone with the module now (look for the board's\n"
          "Bluetooth name). Waiting up to 60 s for the module to report a connection...")
    ok, st = wait_state(console, "connected", lambda v: v == "yes", 60)
    print(f"  connected : {st.get('connected', '?')}   phone: '{st.get('phone name', '')}'")
    return ok


def step_incoming(console) -> bool:
    print("Call the paired phone from another phone. Don't answer on the phone.\n"
          "(The Analog Redial bell should ring too -- that's the real feature.)")
    ok, st = wait_state(console, "call", lambda v: v == "incoming", 45)
    print(f"  call      : {st.get('call', '?')}   caller id: '{st.get('caller id', '')}'   "
          f"last status: {st.get('last status', '?')}")
    return ok


def step_hangup(console) -> bool:
    print("Now hang up from the CALLING side (or answer on the handset then hang up).")
    ok, st = wait_state(console, "call", lambda v: v == "idle", 60)
    print(f"  call      : {st.get('call', '?')}   last status: {st.get('last status', '?')}")
    if not ok:
        print("  The state never returned to idle: note what 'last status' says and what the\n"
              "  module sent (bk_listen 10000 while hanging up) -- see 'call end' in bk3266.md.")
    return ok


RUN = {"selftest": step_selftest, "probe": step_probe, "link": step_link,
       "pairing": step_pairing, "incoming": step_incoming, "hangup": step_hangup}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--only", choices=STEPS, help="run a single step")
    args = parser.parse_args()
    port = pick_port(args.port)
    print(f"Using port: {port}")

    with PhoneConsole(port, args.baud) as console:
        if not console.is_alive():
            print("FAIL: no console response -- is the board flashed and the port right?")
            return 1
        for name in ([args.only] if args.only else STEPS):
            print(f"\n=== {name} ===")
            ok = RUN[name](console)
            print(f"{'PASS' if ok else 'FAIL'}: {name}")
            if not ok:
                return 1
    print("\nAll steps passed. Now the audio checks by ear (see bk3266.md, 'Audio').")
    return 0


if __name__ == "__main__":
    sys.exit(main())
