#!/usr/bin/env python3
"""A stand-in softphone for testing tiny_sip_proxy.py (or a real PBX) against
the board: registers, calls a number, waits for it to be answered, exchanges
a few seconds of G.711 audio packets, hangs up, and says what it saw.

  ./sip_test_call.py --server 192.168.12.114 --to 1000

Answer the call on the board (lift that handset, or `tinject offhook 0` in the
console's test mode) within --wait seconds. No real audio is sent (silence), but
it counts the packets the board sends back, which is how it tells that the
audio path works.
"""

from __future__ import annotations

import argparse
import struct
import random
import re
import select
import socket
import string
import sys
import time
import wave


def _ulaw(b: int) -> int:
    b = ~b & 0xFF
    t = (((b & 0x0F) << 3) + 0x84) << ((b & 0x70) >> 4)
    return (0x84 - t) if b & 0x80 else (t - 0x84)


ULAW = [_ulaw(i) for i in range(256)]


def tok(n: int = 10) -> str:
    return "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(n))


def headers_of(text: str) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for line in text.split("\r\n\r\n")[0].split("\r\n")[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            out.setdefault(k.strip().lower(), []).append(v.strip())
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", required=True, help="the proxy / PBX address")
    ap.add_argument("--to", default="1000", help="number to call (default 1000 = the board's line 0)")
    ap.add_argument("--user", default="2000", help="extension to register as")
    ap.add_argument("--wait", type=float, default=30, help="seconds to wait for the call to be answered")
    ap.add_argument("--talk", type=float, default=4, help="seconds of audio to exchange once answered")
    ap.add_argument("--mute", action="store_true", help="send no audio packets (a caller whose mic sends nothing)")
    ap.add_argument("--save", metavar="FILE.wav", help="write the audio the board sends to a 8 kHz mono WAV file")
    args = ap.parse_args()

    me = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    me.connect((args.server, 5060))
    my_ip = me.getsockname()[0]
    me.close()
    sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sip.bind((my_ip, 0))
    sip_port = sip.getsockname()[1]
    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind((my_ip, 0))
    rtp_port = rtp.getsockname()[1]
    server = (args.server, 5060)
    contact = f"<sip:{args.user}@{my_ip}:{sip_port}>"
    from_tag, call_id = tok(8), f"{tok(12)}@{my_ip}"

    def send(text: str) -> None:
        sip.sendto(text.encode("latin-1"), server)

    def recv(timeout: float) -> str | None:
        r, _, _ = select.select([sip], [], [], timeout)
        return sip.recvfrom(4096)[0].decode("latin-1") if r else None

    # ---- REGISTER ----
    reg = (f"REGISTER sip:{args.server} SIP/2.0\r\nVia: SIP/2.0/UDP {my_ip}:{sip_port};branch=z9hG4bK{tok()};rport\r\n"
           f"Max-Forwards: 70\r\nFrom: <sip:{args.user}@{args.server}>;tag={from_tag}\r\nTo: <sip:{args.user}@{args.server}>\r\n"
           f"Call-ID: reg-{call_id}\r\nCSeq: 1 REGISTER\r\nContact: {contact}\r\nExpires: 120\r\nContent-Length: 0\r\n\r\n")
    send(reg)
    resp = recv(3)
    print("REGISTER ->", resp.split("\r\n")[0] if resp else "no answer")
    if not resp or " 200 " not in resp.split("\r\n")[0]:
        return 1

    # ---- INVITE ----
    sdp = (f"v=0\r\no=- 1 1 IN IP4 {my_ip}\r\ns=test\r\nc=IN IP4 {my_ip}\r\nt=0 0\r\n"
           f"m=audio {rtp_port} RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n")
    ruri = f"sip:{args.to}@{args.server}"
    inv_branch = "z9hG4bK" + tok()
    inv = (f"INVITE {ruri} SIP/2.0\r\nVia: SIP/2.0/UDP {my_ip}:{sip_port};branch={inv_branch};rport\r\nMax-Forwards: 70\r\n"
           f"From: <sip:{args.user}@{args.server}>;tag={from_tag}\r\nTo: <{ruri}>\r\nCall-ID: {call_id}\r\n"
           f"CSeq: 1 INVITE\r\nContact: {contact}\r\nContent-Type: application/sdp\r\nContent-Length: {len(sdp)}\r\n\r\n{sdp}")
    send(inv)
    print(f"INVITE {ruri} sent; answer it on the board (up to {args.wait:.0f} s)...")
    deadline, final = time.time() + args.wait, None
    while time.time() < deadline:
        r = recv(1)
        if not r:
            continue
        status = r.split("\r\n")[0]
        if status.startswith("SIP/2.0"):
            code = int(status.split()[1])
            print("  <-", status)
            if code >= 200:
                final = r
                break
        elif r.startswith("BYE"):
            return 1
    if not final or not final.split("\r\n")[0].startswith("SIP/2.0 2"):
        print("call not answered" if not final else "call failed")
        if not final:
            send(inv.replace("INVITE", "CANCEL", 1).replace("CSeq: 1 INVITE", "CSeq: 1 CANCEL").split("Content-Type")[0] + "Content-Length: 0\r\n\r\n")
            time.sleep(0.5)
        return 2

    # ---- answered: ACK, media, BYE ----
    h = headers_of(final)
    to_hdr = h["to"][0]
    remote_target = re.search(r"<([^>]*)>", h["contact"][0]).group(1)
    routes = [re.search(r"<([^>]*)>", x).group(1) for x in reversed(re.findall(r"<[^>]*>", ", ".join(h.get("record-route", []))))]
    m = re.search(r"c=IN IP4 (\S+)", final)
    p = re.search(r"m=audio (\d+)", final)
    remote_rtp = (m.group(1), int(p.group(1))) if m and p else None
    route_hdr = "".join(f"Route: <{x}>\r\n" for x in routes)
    send(f"ACK {remote_target} SIP/2.0\r\nVia: SIP/2.0/UDP {my_ip}:{sip_port};branch=z9hG4bK{tok()};rport\r\nMax-Forwards: 70\r\n"
         f"{route_hdr}From: <sip:{args.user}@{args.server}>;tag={from_tag}\r\nTo: {to_hdr}\r\nCall-ID: {call_id}\r\n"
         f"CSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n")
    print("answered; route set:", routes or "(none)", "| board audio at", remote_rtp)

    sent = got = 0
    pcm = bytearray()
    seq, ts, t_end = 0, 0, time.time() + args.talk
    while time.time() < t_end:
        if remote_rtp and not args.mute:
            rtp.sendto(bytes([0x80, 0x00]) + seq.to_bytes(2, "big") + ts.to_bytes(4, "big") + (0x1234).to_bytes(4, "big") + b"\xff" * 160, remote_rtp)
            sent += 1
            seq, ts = (seq + 1) & 0xFFFF, ts + 160
        r, _, _ = select.select([rtp, sip], [], [], 0.02)
        for s in r:
            if s is rtp:
                pkt = rtp.recvfrom(2048)[0]
                got += 1
                if len(pkt) > 12 and pkt[1] & 0x7F == 0:
                    pcm += struct.pack(f"<{len(pkt) - 12}h", *(ULAW[b] for b in pkt[12:]))
            else:
                msg = sip.recvfrom(4096)[0].decode("latin-1")
                if msg.startswith("BYE"):
                    hh = headers_of(msg)
                    sip.sendto(("SIP/2.0 200 OK\r\n" + "".join(f"Via: {v}\r\n" for v in hh["via"]) + f"From: {hh['from'][0]}\r\nTo: {hh['to'][0]}\r\n"
                                f"Call-ID: {hh['call-id'][0]}\r\nCSeq: {hh['cseq'][0]}\r\nContent-Length: 0\r\n\r\n").encode(), server)
                    print("the board hung up first")
                    t_end = 0
        time.sleep(0.005)
    send(f"BYE {remote_target} SIP/2.0\r\nVia: SIP/2.0/UDP {my_ip}:{sip_port};branch=z9hG4bK{tok()};rport\r\nMax-Forwards: 70\r\n"
         f"{route_hdr}From: <sip:{args.user}@{args.server}>;tag={from_tag}\r\nTo: {to_hdr}\r\nCall-ID: {call_id}\r\n"
         f"CSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n")
    r = recv(3)
    print("BYE ->", r.split("\r\n")[0] if r else "no answer")
    print(f"audio: sent {sent} packets, received {got} from the board")
    if args.save:
        with wave.open(args.save, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(8000)
            w.writeframes(bytes(pcm))
        print("saved", args.save)
    return 0 if got else 3


if __name__ == "__main__":
    sys.exit(main())
