#!/usr/bin/env python3
"""Synthesise a dial-up modem connect sound (the V.8 / V.34 handshake) as a
mono 16-bit WAV for the phone, 8000 Hz by default.

Everything is generated from the signals the ITU recommendations describe,
so the result is ours and can ship:

  answer tone     2100 Hz, 15 Hz amplitude modulation, phase flips every 450 ms (V.25 / V.8 ANSam)
  V.8 exchange    300 baud FSK, caller on 980/1180 Hz and answerer on 1650/1850 Hz, both at once (V.21)
  INFO0           1200 Hz and 2400 Hz tones with phase reversals, the "beep-beep" (V.34)
  line probing    21 tones from 150 Hz to 3750 Hz in 150 Hz steps, the "bong" (V.34 L1/L2)
  training        2400 baud QAM on 1800 Hz, the hiss; it shifts carrier and level part way (V.34)

Stdlib only. Usage:
  gen_modem.py [--out FILE] [--rate 8000] [--seed N] [--upload HOST]
"""
from __future__ import annotations

import argparse
import math
import random
import struct
import subprocess
import wave
from pathlib import Path

TAU = 2 * math.pi


class Track:
    def __init__(self, rate: int):
        self.rate = rate
        self.s: list[float] = []

    def at(self, t: float) -> int:
        return int(t * self.rate)

    def mix(self, start_s: float, samples: list[float]) -> None:
        i0 = self.at(start_s)
        if len(self.s) < i0 + len(samples):
            self.s.extend([0.0] * (i0 + len(samples) - len(self.s)))
        for i, v in enumerate(samples):
            self.s[i0 + i] += v


def ramp(x: list[float], rate: int, ms: float = 8) -> list[float]:
    n = min(len(x) // 2, int(rate * ms / 1000))
    for i in range(n):
        g = i / n
        x[i] *= g
        x[-1 - i] *= g
    return x


def answer_tone(rate: int, secs: float) -> list[float]:
    out, ph = [], 0.0
    for i in range(int(secs * rate)):
        t = i / rate
        flip = math.pi * (int(t / 0.45) % 2)
        am = 1 + 0.2 * math.sin(TAU * 15 * t)
        out.append(0.35 * am * math.sin(TAU * 2100 * t + flip))
    return ramp(out, rate)


def fsk(rate: int, secs: float, mark: float, space: float, rng: random.Random, level: float) -> list[float]:
    out, ph = [], 0.0
    spb = rate / 300
    bit = 1
    for i in range(int(secs * rate)):
        if i % int(spb) == 0:
            bit = rng.random() < 0.55
        ph += TAU * (mark if bit else space) / rate
        out.append(level * math.sin(ph))
    return ramp(out, rate)


def info0(rate: int, secs: float) -> list[float]:
    out = []
    for i in range(int(secs * rate)):
        t = i / rate
        f = 1200 if int(t / 0.12) % 2 == 0 else 2400
        flip = math.pi * (int(t / 0.06) % 2)
        out.append(0.3 * math.sin(TAU * f * t + flip))
    return ramp(out, rate, 4)


def line_probe(rate: int, secs: float, rng: random.Random) -> list[float]:
    tones = [(150 * k, rng.random() * TAU) for k in range(1, 26) if 150 * k < rate / 2 - 100]
    out = []
    for i in range(int(secs * rate)):
        t = i / rate
        flip = math.pi if t > secs / 2 else 0.0   # L1 then L2, a phase step half way
        out.append(sum(math.sin(TAU * f * t + p + flip) for f, p in tones) * 0.5 / math.sqrt(len(tones)))
    return ramp(out, rate, 15)


def qam(rate: int, secs: float, carrier: float, baud: float, rng: random.Random, level: float) -> list[float]:
    """Random 16-QAM symbols, raised-cosine-ish smoothing between symbols."""
    pts = (-3, -1, 1, 3)
    out = []
    i_prev = q_prev = 0.0
    i_cur, q_cur = rng.choice(pts), rng.choice(pts)
    sps = rate / baud
    next_sym = sps
    for n in range(int(secs * rate)):
        if n >= next_sym:
            i_prev, q_prev = i_cur, q_cur
            i_cur, q_cur = rng.choice(pts), rng.choice(pts)
            next_sym += sps
        frac = 1 - (next_sym - n) / sps
        w = 0.5 - 0.5 * math.cos(math.pi * min(1.0, frac * 1.6))
        i_v = i_prev + (i_cur - i_prev) * w
        q_v = q_prev + (q_cur - q_prev) * w
        t = n / rate
        out.append(level / 4.2 * (i_v * math.cos(TAU * carrier * t) - q_v * math.sin(TAU * carrier * t)))
    return ramp(out, rate, 20)


def bandpass(x: list[float], rate: int, lo: float = 300, hi: float = 3400) -> list[float]:
    """A telephone line: second-order high-pass at lo, then low-pass at hi (RBJ biquads)."""
    def biquad(x, b0, b1, b2, a1, a2):
        y, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
        for v in x:
            o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, v, y1, o
            y.append(o)
        return y

    def coeffs(f, kind):
        w = TAU * min(f, rate / 2 * 0.95) / rate
        a = math.sin(w) / (2 * 0.7071)
        c = math.cos(w)
        if kind == "lp":
            b0, b1, b2 = (1 - c) / 2, 1 - c, (1 - c) / 2
        else:
            b0, b1, b2 = (1 + c) / 2, -(1 + c), (1 + c) / 2
        a0 = 1 + a
        return b0 / a0, b1 / a0, b2 / a0, -2 * c / a0, (1 - a) / a0

    return biquad(biquad(x, *coeffs(lo, "hp")), *coeffs(hi, "lp"))


def build(rate: int, seed: int) -> list[float]:
    rng = random.Random(seed)
    tr = Track(rate)
    t = 0.0
    tr.mix(t, answer_tone(rate, 1.6)); t += 1.75
    # V.8: the caller's CM starts, the answerer's JM joins it, both chatter together
    tr.mix(t, fsk(rate, 0.9, 1180, 980, rng, 0.22))
    tr.mix(t + 0.15, fsk(rate, 0.8, 1850, 1650, rng, 0.25)); t += 1.05
    tr.mix(t, info0(rate, 0.5)); t += 0.55
    tr.mix(t, line_probe(rate, 0.32, rng)); t += 0.38
    tr.mix(t, info0(rate, 0.25)); t += 0.3
    tr.mix(t, line_probe(rate, 0.22, rng)); t += 0.3
    # Training: the answerer's carrier, then both directions at once, the familiar hiss
    tr.mix(t, qam(rate, 0.9, 1800, 2400, rng, 0.35)); t += 0.75
    tr.mix(t, qam(rate, 2.4, 1920, 3200, rng, 0.33))
    tr.mix(t + 0.2, qam(rate, 2.0, 1800, 2400, rng, 0.25)); t += 2.4
    tr.mix(t, [0.0] * int(0.15 * rate))
    x = bandpass(tr.s, rate)
    x = [math.tanh(1.6 * v) for v in x]   # a little line clipping
    peak = max(abs(v) for v in x) or 1
    return [0.7 * v / peak for v in x]   # about -3 dBFS


def write_wav(path: Path, samples: list[float], rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v)) * 32767)) for v in samples))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "voice_prompts" / "modem_connect.wav"))
    p.add_argument("--rate", type=int, default=8000)
    p.add_argument("--seed", type=int, default=1999, help="Changes the random data, so each seed sounds a bit different")
    p.add_argument("--upload", metavar="HOST", help="Also POST it to http://HOST/media/<file name>")
    a = p.parse_args()
    out = Path(a.out)
    s = build(a.rate, a.seed)
    write_wav(out, s, a.rate)
    print(f"wrote {out} ({len(s) / a.rate:.1f} s, {a.rate} Hz mono)")
    if a.upload:
        subprocess.run(["curl", "-s", "-X", "POST", "-H", "Content-Type: application/octet-stream",
                        "--data-binary", f"@{out}", f"http://{a.upload}/media/{out.name}"], check=True)
        print(f"\nuploaded to {a.upload}")


if __name__ == "__main__":
    main()
