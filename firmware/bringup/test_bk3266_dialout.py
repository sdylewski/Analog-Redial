#!/usr/bin/env python3
"""Bring-up: can the BK3266 place a call to a number? (Expected answer: no.)

What the documents say: neither the binary serial protocol nor the text AT
firmware has a command that carries a phone number to dial. The only way
the module starts a call is REDIAL (last number the phone dialed). This
script tests that claim on a real module rather than trusting it, and
tries the places an undocumented dial command could be hiding.

NOTE on codes 0x13-0x1C: an earlier note in this repo took those for
"send DTMF digit 0-9". In the serial protocol they are documented queries
and test commands (0x13 phone name, 0x14 version, 0x15 ACL, 0x16 call
status, 0x18/0x19 test tone, 0x1A volume, 0x1B/0x1C transmit-mode search).
The ToolKit's button-action numbering is a different table. This script
does not send them as digits.

Needs: the BK3266 driver running (Settings -> Hardware -> BK3266, reboot,
Settings -> Bluetooth on, 'bt_enable 1'), a phone paired and connected,
and a number you control to dial (your other phone). Nothing here is
destructive by design: factory reset, clear-pairings, test/idle mode and
transmit-mode commands are refused. The undocumented codes are only tried
with --explore, and the worst they're expected to do is drop the pairing.

  ./test_bk3266_dialout.py --number 5551234567            # documented checks only
  ./test_bk3266_dialout.py --number 5551234567 --explore  # + undocumented probes
"""

from __future__ import annotations
import argparse
import re
import sys
import time

from phone_serial import PhoneConsole, add_common_args, pick_port

# Codes that must never be sent by this script (even via --code):
#   0A clear pairings   1D factory reset   1E test mode   1F idle mode
#   22 clear key        24 transmit mode (restarts the module)
FORBIDDEN = {0x0A, 0x1D, 0x1E, 0x1F, 0x22, 0x24}


def hexstr(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def state(console: PhoneConsole) -> dict:
    out = {}
    for line in console.send("bk_state", timeout_s=3):
        m = re.match(r"^\s*([a-z ]+?)\s*:\s*(.*)$", line)
        if m:
            out[m.group(1).strip()] = m.group(2).strip().strip("'")
    return out


def wait_call(console, want, seconds):
    end = time.time() + seconds
    last = {}
    while time.time() < end:
        last = state(console)
        if last.get("call", "") in want:
            return True, last
        time.sleep(0.7)
    return False, last


def ask(prompt: str) -> bool:
    return input(prompt + " [y/N] ").strip().lower().startswith("y")


class Probe:
    def __init__(self, name, why, commands, explore=False, needs_prior_call=False):
        self.name, self.why, self.commands = name, why, commands
        self.explore, self.needs_prior_call = explore, needs_prior_call


def build_probes(number: str) -> list[Probe]:
    digits = number.encode("ascii")
    d = hexstr(digits)
    return [
        Probe("redial", "documented: BT_CMD_REDIAL (0x07), the one way the module starts a call. "
                        "Proves the module CAN originate a call, just not to a chosen number.",
              ['bk_cmd 07'], needs_prior_call=True),
        Probe("redial+digits", "REDIAL (0x07) with the number appended as DATA: does the module "
                               "dial what it is given instead of the last number?",
              [f'bk_frame 01 "07 {d}"'], explore=True),
        Probe("outer-0x02", "outer CMD 0x02 is 'outgoing number' in the module->MCU direction; "
                            "maybe it is bidirectional.",
              [f'bk_frame 02 "{d}"'], explore=True),
        Probe("code-0x26", "0x26 is undocumented (the table jumps 0x25 -> 0x29): bare",
              ['bk_frame 01 "26"'], explore=True),
        Probe("code-0x26+digits", "undocumented 0x26 carrying the number",
              [f'bk_frame 01 "26 {d}"'], explore=True),
        Probe("code-0x27+digits", "undocumented 0x27 carrying the number",
              [f'bk_frame 01 "27 {d}"'], explore=True),
        Probe("code-0x28+digits", "undocumented 0x28 carrying the number",
              [f'bk_frame 01 "28 {d}"'], explore=True),
        Probe("text-ATD", "a text-AT pass-through of the HFP dial command (text firmware only)",
              [f'bk_at ATD{number};'], explore=True),
        Probe("text-BT+CD", "guessed text-AT dial command (text firmware only)",
              [f'bk_at BT+CD{number}'], explore=True),
    ]


def hang_up(console) -> None:
    console.send("bk_cmd 06", timeout_s=3)       # BT_CMD_NACK
    wait_call(console, {"idle"}, 10)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--number", required=True,
                    help="digits to try dialing: a phone YOU control, never a real service")
    ap.add_argument("--explore", action="store_true",
                    help="also send undocumented codes carrying the number")
    ap.add_argument("--code", help="extra: one hex command code to send as a plain command")
    ap.add_argument("--watch", type=float, default=7.0, help="seconds to watch for a call")
    args = ap.parse_args()

    if not re.fullmatch(r"\d{3,20}", args.number):
        ap.error("--number must be 3-20 digits")
    if args.code:
        try:
            c = int(args.code, 16)
        except ValueError:
            ap.error("--code must be one hex byte")
        if c in FORBIDDEN:
            ap.error(f"code {c:02X} is on the never-send list (factory reset / clear / mode change)")

    port = pick_port(args.port)
    print(f"Using port: {port}")
    results: list[tuple[str, str]] = []

    with PhoneConsole(port, args.baud) as console:
        if not console.is_alive():
            print("FAIL: no console response")
            return 1
        st = state(console)
        if st.get("task") != "running":
            print("The BK3266 driver isn't running. Set Settings -> Hardware to BK3266, reboot, "
                  "then 'bt_enable 1'.")
            return 1
        if st.get("connected") != "yes":
            print("No phone connected. Pair one first (test_bk3266.py --only pairing).")
            return 1
        if st.get("call") != "idle":
            print(f"A call is already {st.get('call')}; end it and rerun.")
            return 1
        print(f"Connected to '{st.get('phone name', '?')}'. Will try number {args.number}.")
        print("Keep your phone's call screen in view: a probe 'works' if the PHONE starts dialing.")

        probes = build_probes(args.number)
        if args.code:
            probes.append(Probe(f"your-code-{c:02X}", "your --code, as a plain command",
                                [f"bk_cmd {c:02X}"]))

        for p in probes:
            if p.explore and not args.explore:
                results.append((p.name, "skipped (needs --explore)"))
                continue
            print(f"\n--- {p.name} ---\n{p.why}")
            if p.needs_prior_call:
                print(f"First, on the phone: call {args.number} yourself, let it ring once, hang up.")
                if not ask("Done?"):
                    results.append((p.name, "skipped"))
                    continue
            else:
                if not ask(f"Send this? ({' ; '.join(p.commands)})"):
                    results.append((p.name, "skipped"))
                    continue
            for cmd in p.commands:
                print(f"  > {cmd}")
                console.send(cmd, timeout_s=3)
            seen, last = wait_call(console, {"outgoing", "active"}, args.watch)
            human = ask("Did the PHONE start a call?")
            if seen or human:
                note = f"CALL STARTED (module state: {last.get('call', '?')}, " \
                       f"last status {last.get('last status', '?')})"
                if seen != human:
                    note += f" -- module state {'saw' if seen else 'did not see'} it, you " \
                            f"{'saw' if human else 'did not see'} it"
                results.append((p.name, note))
                hang_up(console)
            else:
                results.append((p.name, f"no call (module state {last.get('call', '?')}, "
                                        f"last status {last.get('last status', '?')})"))

    print("\n=== summary ===")
    width = max(len(n) for n, _ in results)
    for name, res in results:
        print(f"  {name:<{width}}  {res}")
    started = [n for n, r in results if r.startswith("CALL STARTED")]
    others = [n for n in started if n != "redial"]
    print()
    if others:
        print("A probe other than REDIAL started a call: that's a dial-out path. Write the exact "
              "command and number format in archive/hardware/bk3266.md (open question 11).")
    elif "redial" in started:
        print("Only REDIAL starts a call. The module can call the last number the phone dialed, "
              "and nothing else: no dial-out to a chosen number.")
    else:
        print("Nothing started a call, not even REDIAL. Check REDIAL's precondition (the phone "
              "must have a last-dialed number) before concluding anything.")
    print("Record the table above in archive/hardware/bk3266.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
