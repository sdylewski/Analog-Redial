#!/usr/bin/env python3
"""A tiny SIP registrar and proxy, for trying the board's VoIP without a PBX.

Run it on a laptop on the same network, point the board's Settings -> VOIP ->
"SIP server" at the laptop's address, and a softphone registered to the same
address can call the board (and the board can call the softphone).

Why a proxy, not just "call the board's IP": the board only accepts SIP
requests that come from its configured server, including the ACK and BYE in
the middle of a call, and it sends its own BYE to the server too. So the
server has to stay in the signalling path for the whole call (Record-Route).
Audio does not go through here: it flows straight between the two phones.

What it is not: no authentication (anyone on the network can register), no
TCP/TLS, no NAT help, no voicemail, no conferencing. Bench use only.

  ./tiny_sip_proxy.py                     # listens on this machine's address, port 5060
  ./tiny_sip_proxy.py --ip 192.168.0.10   # a specific address
  ./tiny_sip_proxy.py -v                  # print every message

Dialled numbers can be mapped to the names the board registered under
(its "extension" is whatever Settings -> VOIP says, even letters):
  --alias 1000=phone --alias 1001=phone2     (these two are the default)
"""

from __future__ import annotations

import argparse
import asyncio
import random
import re
import socket
import string
import sys
import time

COMPACT = {"v": "via", "f": "from", "t": "to", "i": "call-id", "m": "contact",
           "l": "content-length", "c": "content-type"}
TXN_LIFETIME_S = 180


def rand_token(n: int = 12) -> str:
    return "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(n))


class Msg:
    def __init__(self, first: str, headers: list[list[str]], body: str):
        self.first, self.headers, self.body = first, headers, body  # headers: [lowername, name, value]

    @staticmethod
    def parse(data: bytes) -> "Msg | None":
        text = data.decode("latin-1")
        head, _, body = text.partition("\r\n\r\n")
        if not _:
            head, _, body = text.partition("\n\n")
        lines = head.replace("\r\n", "\n").split("\n")
        if not lines or not lines[0].strip():
            return None
        first, headers = lines[0].strip(), []
        for line in lines[1:]:
            if line[:1] in (" ", "\t") and headers:
                headers[-1][2] += " " + line.strip()
            elif ":" in line:
                name, value = line.split(":", 1)
                name = name.strip()
                headers.append([COMPACT.get(name.lower(), name.lower()), name, value.strip()])
        return Msg(first, headers, body)

    @property
    def is_response(self) -> bool:
        return self.first.startswith("SIP/2.0")

    @property
    def method(self) -> str:
        return self.first.split(" ", 1)[0]

    @property
    def uri(self) -> str:
        return self.first.split(" ")[1]

    @property
    def code(self) -> int:
        return int(self.first.split(" ")[1])

    def all(self, name: str) -> list[str]:
        out: list[str] = []
        for low, _, value in self.headers:
            if low == name:
                out.extend(split_list(value) if name in ("via", "route", "record-route") else [value])
        return out

    def get(self, name: str) -> str | None:
        values = self.all(name)
        return values[0] if values else None

    def remove(self, name: str) -> None:
        self.headers = [h for h in self.headers if h[0] != name]

    def set(self, name: str, value: str) -> None:
        self.remove(name)
        self.headers.append([name, name.title(), value])

    def set_list(self, name: str, values: list[str], at_top: bool = False) -> None:
        self.remove(name)
        rows = [[name, "-".join(p.capitalize() for p in name.split("-")), v] for v in values]
        self.headers = rows + self.headers if at_top else self.headers + rows

    def dump(self) -> bytes:
        self.remove("content-length")
        out = [self.first]
        for _, name, value in self.headers:
            out.append(f"{name}: {value}")
        out.append(f"Content-Length: {len(self.body.encode('latin-1'))}")
        return ("\r\n".join(out) + "\r\n\r\n" + self.body).encode("latin-1")


def split_list(value: str) -> list[str]:
    parts, depth, cur = [], 0, ""
    for ch in value:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def uri_of(value: str) -> str:
    m = re.search(r"<([^>]*)>", value)
    return (m.group(1) if m else value.split(";")[0]).strip()


def uri_parts(uri: str) -> tuple[str, str, int]:
    """sip:user@host:port;params -> (user, host, port)."""
    m = re.match(r"sips?:(?:([^@;]*)@)?([^:;>]+)(?::(\d+))?", uri.strip())
    if not m:
        return "", "", 5060
    return m.group(1) or "", m.group(2), int(m.group(3) or 5060)


def via_parts(via: str) -> tuple[str, int, dict[str, str]]:
    m = re.match(r"SIP/2\.0/\w+\s+([^;\s]+)((?:;[^;]*)*)", via.strip(), re.I)
    sent_by, params = (m.group(1), m.group(2)) if m else ("", "")
    host, _, port = sent_by.partition(":")
    pd: dict[str, str] = {}
    for p in params.split(";"):
        if p:
            k, _, v = p.partition("=")
            pd[k.strip().lower()] = v.strip()
    return host, int(port or 5060), pd


class Txn:
    def __init__(self, fwd_branch: str, dest: tuple[str, int], ruri: str, client: tuple[str, int],
                 client_vias: list[str]):
        self.fwd_branch, self.dest, self.ruri, self.client = fwd_branch, dest, ruri, client
        self.client_vias = client_vias   # the sender's Via headers, to rebuild a reply that lost them
        self.final_non2xx = False
        self.created = time.time()


class Proxy(asyncio.DatagramProtocol):
    def __init__(self, ip: str, port: int, aliases: dict[str, str], verbose: bool):
        self.ip, self.port, self.aliases, self.verbose = ip, port, aliases, verbose
        self.registry: dict[str, dict] = {}
        self.txn_by_client: dict[tuple[str, str], Txn] = {}
        self.txn_by_fwd: dict[str, Txn] = {}
        self.transport: asyncio.DatagramTransport | None = None

    # ---- plumbing ----
    def connection_made(self, transport):  # type: ignore[override]
        self.transport = transport

    def log(self, text: str) -> None:
        print(time.strftime("%H:%M:%S"), text, flush=True)

    def send(self, msg: Msg, dest: tuple[str, int]) -> None:
        data = msg.dump()
        if self.verbose:
            self.log(f"--> {dest[0]}:{dest[1]}\n" + data.decode("latin-1").replace("\r\n", "\n"))
        assert self.transport
        self.transport.sendto(data, dest)

    def datagram_received(self, data: bytes, addr):  # type: ignore[override]
        msg = Msg.parse(data)
        if not msg:
            return
        if self.verbose:
            self.log(f"<-- {addr[0]}:{addr[1]}\n" + data.decode("latin-1").replace("\r\n", "\n"))
        self.expire_txns()
        try:
            if msg.is_response:
                self.on_response(msg, addr)
            else:
                self.on_request(msg, addr)
        except Exception as e:  # keep serving whatever happens to one message
            self.log(f"!! error handling {msg.first!r} from {addr}: {e!r}")

    def expire_txns(self) -> None:
        now = time.time()
        for key, t in list(self.txn_by_client.items()):
            if now - t.created > TXN_LIFETIME_S:
                del self.txn_by_client[key]
                self.txn_by_fwd.pop(t.fwd_branch, None)

    def make_response(self, req: Msg, code: int, reason: str, extra: list[tuple[str, str]] = (), body: str = "") -> Msg:
        r = Msg(f"SIP/2.0 {code} {reason}", [], body)
        for v in req.all("via"):
            r.headers.append(["via", "Via", v])
        for name in ("from", "to", "call-id", "cseq"):
            r.headers.append([name, name.title() if name != "call-id" else "Call-ID", req.get(name) or ""])
        if code >= 200:
            to = req.get("to") or ""
            if "tag=" not in to:
                r.set("to", to + ";tag=" + rand_token(8))
        for k, v in extra:
            r.headers.append([k.lower(), k, v])
        return r

    def mark_via(self, req: Msg, addr) -> None:
        """RFC 3261 18.2.1 / RFC 3581: note where the request really came from."""
        vias = req.all("via")
        if not vias:
            return
        host, _, pd = via_parts(vias[0])
        top = vias[0]
        if host != addr[0]:
            top += f";received={addr[0]}"
        if "rport" in pd and not pd["rport"]:
            top = re.sub(r";rport(?![=\w])", f";rport={addr[1]}", top)
        req.set_list("via", [top] + vias[1:], at_top=True)

    # ---- requests ----
    def on_request(self, m: Msg, addr) -> None:
        method = m.method
        self.mark_via(m, addr)
        if method == "REGISTER":
            return self.register(m, addr)
        if method == "OPTIONS" and uri_parts(m.uri)[1] in (self.ip, "") :
            user = self.resolve(uri_parts(m.uri)[0])
            if not user or user in self.registry:
                return self.send(self.make_response(m, 200, "OK", [("Allow", "INVITE, ACK, CANCEL, BYE, OPTIONS")]), addr)
            return self.send(self.make_response(m, 404, "Not Found"), addr)
        if method == "CANCEL":
            return self.cancel(m, addr)
        if method == "ACK":
            return self.ack(m, addr)
        self.forward(m, addr)

    def resolve(self, user: str) -> str:
        return self.aliases.get(user, user)

    def register(self, m: Msg, addr) -> None:
        to_user = uri_parts(uri_of(m.get("to") or ""))[0]
        contact = m.get("contact") or ""
        expires = int(m.get("expires") or 3600)
        mm = re.search(r";expires=(\d+)", contact)
        if mm:
            expires = int(mm.group(1))
        if contact.strip() == "*" or expires == 0:
            self.registry.pop(to_user, None)
            self.log(f"unregistered {to_user}")
        else:
            curi = uri_of(contact)
            _, host, port = uri_parts(curi)
            self.registry[to_user] = {"uri": curi, "addr": (host, port), "until": time.time() + expires}
            self.log(f"registered {to_user} at {host}:{port} for {expires}s")
        reply = self.make_response(m, 200, "OK", [("Contact", contact if contact.strip() != "*" else "*"),
                                                  ("Expires", str(expires))])
        self.send(reply, addr)

    def lookup(self, user: str) -> dict | None:
        # a name that is registered itself wins; otherwise try its alias
        for name in (user, self.resolve(user)):
            reg = self.registry.get(name)
            if reg and reg["until"] < time.time():
                self.registry.pop(name, None)
                reg = None
            if reg:
                return reg
        return None

    def pop_own_route(self, m: Msg) -> None:
        routes = m.all("route")
        if routes:
            _, host, port = uri_parts(uri_of(routes[0]))
            if host == self.ip and port == self.port:
                m.set_list("route", routes[1:], at_top=True) if routes[1:] else m.remove("route")

    def forward(self, m: Msg, addr) -> None:
        method = m.method
        if int(m.get("max-forwards") or 70) <= 1:
            return self.send(self.make_response(m, 483, "Too Many Hops"), addr)
        self.pop_own_route(m)
        user, host, port = uri_parts(m.uri)
        dest_uri, dest = m.uri, (host, port)
        if host == self.ip and port == self.port or host == self.ip:
            reg = self.lookup(user)
            if not reg:
                self.log(f"{method} for {user!r}: not registered -> 404")
                return self.send(self.make_response(m, 404, "Not Found"), addr)
            dest_uri, dest = reg["uri"], reg["addr"]
        top = m.all("via")[0] if m.all("via") else ""
        client_branch = via_parts(top)[2].get("branch", "")
        key = (client_branch, method)
        txn = self.txn_by_client.get(key)
        new_txn = txn is None
        if new_txn:
            txn = Txn("z9hG4bK" + rand_token(), dest, dest_uri, addr, m.all("via"))
            self.txn_by_client[key] = txn
            self.txn_by_fwd[txn.fwd_branch] = txn
        out = Msg(f"{method} {txn.ruri} SIP/2.0", [list(h) for h in m.headers], m.body)
        out.set("max-forwards", str(int(m.get("max-forwards") or 70) - 1))
        out.set_list("via", [f"SIP/2.0/UDP {self.ip}:{self.port};branch={txn.fwd_branch}"] + m.all("via"), at_top=True)
        if method == "INVITE":
            out.set_list("record-route", [f"<sip:{self.ip}:{self.port};lr>"] + m.all("record-route"), at_top=True)
        self.log(f"{method} {m.get('from') and uri_of(m.get('from'))} -> {txn.ruri}")
        if method == "INVITE" and new_txn:
            self.send(self.make_response(m, 100, "Trying"), addr)
        self.send(out, txn.dest)

    def ack(self, m: Msg, addr) -> None:
        self.pop_own_route(m)
        top = m.all("via")[0] if m.all("via") else ""
        txn = self.txn_by_client.get((via_parts(top)[2].get("branch", ""), "INVITE"))
        if txn and txn.final_non2xx:   # ACK for a failed call: hop by hop, same branch as our INVITE
            out = Msg(f"ACK {txn.ruri} SIP/2.0", [list(h) for h in m.headers], m.body)
            out.set_list("via", [f"SIP/2.0/UDP {self.ip}:{self.port};branch={txn.fwd_branch}"], at_top=True)
            return self.send(out, txn.dest)
        user, host, port = uri_parts(m.uri)       # ACK for a 2xx: its own transaction
        dest = (host, port)
        ruri = m.uri
        if host == self.ip:
            reg = self.lookup(user)
            if not reg:
                return
            dest, ruri = reg["addr"], reg["uri"]
        out = Msg(f"ACK {ruri} SIP/2.0", [list(h) for h in m.headers], m.body)
        out.set_list("via", [f"SIP/2.0/UDP {self.ip}:{self.port};branch=z9hG4bK{rand_token()}"] + m.all("via"), at_top=True)
        out.set("max-forwards", str(int(m.get("max-forwards") or 70) - 1))
        self.send(out, dest)

    def cancel(self, m: Msg, addr) -> None:
        top = m.all("via")[0] if m.all("via") else ""
        txn = self.txn_by_client.get((via_parts(top)[2].get("branch", ""), "INVITE"))
        self.send(self.make_response(m, 200, "OK"), addr)
        if not txn:
            return
        out = Msg(f"CANCEL {txn.ruri} SIP/2.0", [list(h) for h in m.headers], "")
        out.set_list("via", [f"SIP/2.0/UDP {self.ip}:{self.port};branch={txn.fwd_branch}"], at_top=True)
        self.log(f"CANCEL -> {txn.ruri}")
        self.send(out, txn.dest)

    # ---- responses ----
    def on_response(self, m: Msg, addr) -> None:
        vias = m.all("via")
        if not vias:
            return
        host, port, pd = via_parts(vias[0])
        if host != self.ip or port != self.port:
            return   # not ours
        cseq = m.get("cseq") or ""
        txn = self.txn_by_fwd.get(pd.get("branch", ""))
        rest = vias[1:]
        # The board's SIP code echoes only the top Via in its replies (and no
        # Record-Route), so rebuild what it dropped from our record of the request.
        if txn and len(rest) < len(txn.client_vias):
            rest = list(txn.client_vias)
        if not rest:
            return
        if txn and "INVITE" in cseq and m.code >= 300:
            txn.final_non2xx = True
        if txn and "INVITE" in cseq and 101 <= m.code < 300 and not m.all("record-route"):
            m.set_list("record-route", [f"<sip:{self.ip}:{self.port};lr>"], at_top=True)
        m.set_list("via", rest, at_top=True)
        h, p, params = via_parts(rest[0])
        dest = (params.get("received") or h, int(params["rport"]) if params.get("rport") else p)
        self.log(f"{m.code} {m.first.split(' ', 2)[2] if len(m.first.split(' ', 2)) > 2 else ''} ({cseq}) -> {dest[0]}:{dest[1]}")
        self.send(m, dest)


def detect_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))   # no packet is sent; just picks the outgoing interface
        return s.getsockname()[0]
    finally:
        s.close()


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", help="address to listen on and put in Via/Record-Route (default: this machine's LAN address)")
    ap.add_argument("--port", type=int, default=5060)
    ap.add_argument("--alias", action="append", default=[], metavar="NUMBER=NAME",
                    help="dial NUMBER to reach the phone registered as NAME (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every message")
    args = ap.parse_args()
    ip = args.ip or detect_ip()
    aliases = {"1000": "phone", "1001": "phone2"}
    for a in args.alias:
        k, _, v = a.partition("=")
        aliases[k] = v
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: Proxy(ip, args.port, aliases, args.verbose), local_addr=(ip, args.port))
    print(f"SIP proxy listening on {ip}:{args.port}  (Ctrl+C to stop)")
    print("aliases:", ", ".join(f"{k} -> {v}" for k, v in aliases.items()))
    print(f"Board: set Settings -> VOIP -> SIP server to {ip} and reboot it.")
    print(f"Softphone: server/domain {ip}, any username (e.g. 2000), no password, UDP, G.711 (PCMU).")
    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
