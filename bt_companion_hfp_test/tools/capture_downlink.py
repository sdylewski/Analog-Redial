#!/usr/bin/env python3
"""Capture raw incoming HFP audio from the ESP32 companion and analyze it.

Sends `capture_downlink <ms>` over the companion's USB serial console, which
records the PCM samples Bluedroid hands the firmware -- BEFORE any 16->8-bit
conversion or DAC output -- and dumps them as base64. This script decodes
that into WAV files and prints a spectral breakdown.

Outputs (in --out-dir):
  raw.wav          exactly what the phone/Bluedroid delivered
  raw_8bit_sim.wav the same audio run through the firmware's own 16->8-bit
                   conversion and back, i.e. what the DAC is being asked to
                   play -- listen to both: if raw.wav sounds fine and the
                   8-bit one sounds "underwater", quantization is the culprit;
                   if raw.wav is already bad, the problem is upstream of this
                   board entirely.

Needs a live HFP call actively sending audio during the capture window, and
nothing else (e.g. `screen`) holding the serial port.

Usage: capture_downlink.py [--port /dev/cu.usbserial-0001] [--ms 4000]
"""
import argparse
import base64
import re
import sys
import time
import wave
from pathlib import Path

import numpy as np
import serial

SAMPLE_RATE = 8000
BEGIN = "---BEGIN PCM16 MONO 8000HZ BASE64---"
END = "---END PCM16---"
B64_LINE = re.compile(r"^[A-Za-z0-9+/=]+$")


def capture(port, ms):
    s = serial.Serial(port, 115200, timeout=0.5)
    s.dtr = False
    s.rts = False
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(f"capture_downlink {ms}\n".encode())
    buf = bytearray()
    # capture time + ~12s for the base64 dump at 115200 baud
    deadline = time.time() + ms / 1000 + 25
    while time.time() < deadline:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
            if END.encode() in buf:
                break
    s.close()
    return buf.decode(errors="replace")


def extract_pcm(text):
    if BEGIN not in text:
        for line in text.splitlines():
            if "capture" in line or "nothing" in line or "malloc" in line:
                print("  device:", line.strip())
        sys.exit("no capture block found in serial output (see device lines above)")
    body = text.split(BEGIN, 1)[1].split(END, 1)[0]
    # BT stack log lines can interleave with the dump -- keep only pure
    # base64 lines
    b64 = "".join(l.strip() for l in body.splitlines() if B64_LINE.match(l.strip()))
    raw = base64.b64decode(b64)
    return np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<i2")


def write_wav(path, samples):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(samples.astype("<i2").tobytes())


def to_8bit_and_back(x):
    # exactly bt_audio_io_write_downlink()'s conversion, then reversed
    u8 = ((x.astype(np.int32) >> 8) + 128).astype(np.uint8)
    return ((u8.astype(np.int32) - 128) << 8).astype(np.int16)


def dbfs(v):
    return 20 * np.log10(max(v, 1e-9) / 32768)


def analyze(x, label):
    x = x.astype(np.float64)
    peak = np.max(np.abs(x))
    rms = np.sqrt(np.mean(x ** 2))
    clipped = int(np.sum(np.abs(x) >= 32767))
    print(f"\n== {label}: {len(x)} samples ({len(x)/SAMPLE_RATE:.2f}s)")
    print(f"   peak {peak:.0f} ({dbfs(peak):.1f} dBFS)  rms {rms:.0f} ({dbfs(rms):.1f} dBFS)"
          f"  clipped samples: {clipped}")
    # how many of the 256 DAC steps this audio actually exercises
    steps = len(np.unique((x.astype(np.int32) >> 8)))
    print(f"   distinct 8-bit DAC levels used: {steps} of 256")

    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    freqs = np.fft.rfftfreq(len(x), 1 / SAMPLE_RATE)
    total = spec.sum() or 1
    print("   energy by band (a steep drop above ~1-2kHz = low-pass character):")
    for lo, hi in [(0, 300), (300, 1000), (1000, 2000), (2000, 3000), (3000, 4000)]:
        e = spec[(freqs >= lo) & (freqs < hi)].sum() / total
        print(f"     {lo:>4}-{hi:<4} Hz  {100*e:5.1f}%  {'#' * int(e * 60)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--ms", type=int, default=4000)
    ap.add_argument("--out-dir", default=".")
    args = ap.parse_args()

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    print(f"capturing {args.ms}ms from {args.port} -- keep the call active...")
    text = capture(args.port, args.ms)
    pcm = extract_pcm(text)
    if len(pcm) == 0:
        sys.exit("capture block was empty")

    sim = to_8bit_and_back(pcm)
    write_wav(out / "raw.wav", pcm)
    write_wav(out / "raw_8bit_sim.wav", sim)
    print(f"wrote {out/'raw.wav'} and {out/'raw_8bit_sim.wav'}")

    analyze(pcm, "raw (what Bluetooth delivered)")
    analyze(sim, "after firmware's 16->8-bit conversion")


if __name__ == "__main__":
    main()
