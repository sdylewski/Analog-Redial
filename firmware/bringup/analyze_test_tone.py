#!/usr/bin/env python3
"""Analyze a captured WAV against a gen_test_tone.py reference, looking for
dropouts/glitches introduced somewhere in the audio path (network, decode,
DMA buffering) rather than doing general speech-quality analysis.

Per-segment check (same idea as the DTMF detector's Goertzel-based energy
check -- this is deliberately simple, not a full spectral analysis): for
each expected tone segment, is there real energy at that frequency, roughly
where it should be in time? A segment with the expected tone at a
reasonable level is OK; one that's silent or far too quiet where a tone
should be is a likely dropout. Timing is aligned once, up front, by finding
the clip's distinctive sync tone in the capture -- so this tolerates some
constant delay (DMA buffering, network latency) without needing the
recording to start at exactly the right instant.

Usage:
    python3 analyze_test_tone.py captured.wav --reference test_tone.json

Exit code 0 if every segment looks fine, 1 if any segment looks dropped or
the sync tone couldn't be found at all (capture unusable/misaligned).
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import wave
from pathlib import Path


def read_wav_mono16(path: Path) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as w:
        sr = w.getframerate()
        n = w.getnframes()
        sampwidth = w.getsampwidth()
        channels = w.getnchannels()
        raw = w.readframes(n)
    if sampwidth != 2:
        raise ValueError(f"expected 16-bit PCM, got {sampwidth * 8}-bit")
    samples = list(struct.unpack(f"<{n * channels}h", raw))
    if channels > 1:
        # Downmix by taking channel 0 -- captures from this project's audio
        # pipeline are always mono already, but tolerate a stray stereo file.
        samples = samples[0::channels]
    return samples, sr


def goertzel_power(samples: list[float], sample_rate: int, freq: float) -> float:
    """Goertzel magnitude-squared for `freq` across `samples`. Same algorithm
    as dtmf.c's detector (see that file's own comment for why Goertzel over
    an FFT here: one frequency of interest, not a whole spectrum)."""
    n = len(samples)
    if n == 0 or freq <= 0 or freq >= sample_rate / 2:
        return 0.0
    k = int(0.5 + n * freq / sample_rate)
    w = 2 * math.pi * k / n
    coeff = 2 * math.cos(w)
    q1 = q2 = 0.0
    for s in samples:
        q0 = coeff * q1 - q2 + s
        q2 = q1
        q1 = q0
    return q1 * q1 + q2 * q2 - coeff * q1 * q2


def rms(samples: list[int]) -> float:
    if not samples:
        return 0.0
    return math.sqrt(sum(float(s) * s for s in samples) / len(samples))


def find_sync_offset_ms(samples: list[int], sample_rate: int, sync_hz: float,
                         sync_ms: int, search_window_s: float = 4.0) -> float | None:
    """Slides a sync_ms-wide window across the first `search_window_s` of the
    capture in 10ms steps, returns the start time (ms) of the window with
    the strongest sync_hz energy -- None if nothing crosses a sane
    noise-floor multiple anywhere in the search range (capture doesn't
    contain the clip at all, or is silent)."""
    win_n = int(sample_rate * sync_ms / 1000)
    step_n = int(sample_rate * 0.01)
    search_n = min(len(samples), int(sample_rate * search_window_s))

    # Trim the fade edges (see gen_test_tone.py's render_wav) so the window
    # measures steady-state tone, not the ramp.
    trim = win_n // 8

    best_power = -1.0
    best_start = 0
    powers = []
    pos = 0
    while pos + win_n <= search_n:
        window = samples[pos + trim: pos + win_n - trim]
        p = goertzel_power([float(s) for s in window], sample_rate, sync_hz)
        powers.append(p)
        if p > best_power:
            best_power = p
            best_start = pos
        pos += step_n

    if not powers:
        return None
    # Require the best position to clearly stand out from the search
    # window's own median power -- otherwise there's no real sync tone here,
    # just noise the Goertzel happened to respond to somewhere.
    powers.sort()
    median = powers[len(powers) // 2]
    if median <= 0 or best_power < median * 10:
        return None
    return best_start * 1000.0 / sample_rate


def analyze(samples: list[int], sample_rate: int, ref: dict, search_window_s: float = 4.0) -> tuple[list[dict], bool]:
    sync_ms = ref["segments"][0]["duration_ms"]
    offset_ms = find_sync_offset_ms(samples, sample_rate, ref["sync_hz"], sync_ms,
                                     search_window_s=search_window_s)
    if offset_ms is None:
        return [], False

    # Noise floor: median RMS across every inter-segment gap (see
    # gen_test_tone.py's GAP_MS), not just one fixed spot. A single window
    # is fragile -- if the sync-offset estimate is off by even a little (the
    # search steps in 10ms increments, and a tone's own decay ring doesn't
    # stop instantly), that one window can clip into real signal and report
    # a falsely high floor, which then wrongly flags every segment as
    # "dropped" even though the segments themselves are clean (confirmed
    # live: this happened, with real segment RMS 9000-16000 all flagged
    # because the single floor sample came out at 11000+). Sampling every
    # gap and taking the median means one bad window can't skew the result.
    gap_ms = 20  # margin trimmed from each edge of the 100ms inter-segment
                 # gap, so a fade-in/out or timing slop can't bleed in
    floor_readings = []
    prev_end_ms = None
    for seg in ref["segments"]:
        seg_start = seg["start_ms"] + offset_ms
        if prev_end_ms is not None and seg_start > prev_end_ms:
            g0 = int(sample_rate * (prev_end_ms + gap_ms) / 1000)
            g1 = int(sample_rate * (seg_start - gap_ms) / 1000)
            if g1 > g0:
                gap_samples = samples[g0:g1]
                if gap_samples:
                    floor_readings.append(rms(gap_samples))
        prev_end_ms = seg_start + seg["duration_ms"]

    if floor_readings:
        floor_readings.sort()
        noise_rms = floor_readings[len(floor_readings) // 2]
    else:
        noise_rms = 1.0
    noise_rms = max(noise_rms, 1.0)  # avoid a zero floor making everything "pass"

    results = []
    all_ok = True
    for seg in ref["segments"]:
        # Skip the sync segment itself -- it already did its job locating
        # t=0, and checking it again against its own frequency is redundant.
        if seg["label"] == "sync":
            continue
        start_ms = seg["start_ms"] + offset_ms
        dur_ms = seg["duration_ms"]
        trim_ms = dur_ms * 0.2  # skip fade in/out + a little margin either side
        win_start = int(sample_rate * (start_ms + trim_ms) / 1000)
        win_end = int(sample_rate * (start_ms + dur_ms - trim_ms) / 1000)
        window = samples[win_start:win_end]

        seg_rms = rms(window)
        power = goertzel_power([float(s) for s in window], sample_rate, seg["freq_hz"])
        # Goertzel power scales with N^2 for a pure tone -- normalize by
        # window length so segments of slightly different sample counts
        # (rate-dependent rounding) are comparable to one flat threshold.
        norm_power = power / max(len(window), 1) ** 2 if window else 0.0

        # A real tone segment's RMS should be well above the noise floor;
        # dropped/silent audio reads close to it. 4x mirrors the DTMF
        # detector's own noise-floor multiple (dtmf.c's MIN_MAG reasoning).
        ok = seg_rms > noise_rms * 4 and norm_power > 0
        if not ok:
            all_ok = False
        results.append({
            "label": seg["label"],
            "freq_hz": seg["freq_hz"],
            "expected_start_ms": seg["start_ms"],
            "rms": seg_rms,
            "noise_floor_rms": noise_rms,
            "ok": ok,
        })
    return results, all_ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captured_wav", help="Recording to check (from `record_downlink`/`record`)")
    parser.add_argument("--reference", required=True, help="JSON sidecar from gen_test_tone.py")
    parser.add_argument("--search-window", type=float, default=4.0,
                         help="Seconds of the capture to search for the sync tone (default: 4.0) -- "
                              "widen this if the recording started well before playback did (e.g. "
                              "manual two-console coordination), up to the capture's own length")
    args = parser.parse_args()

    ref = json.loads(Path(args.reference).read_text())
    samples, sample_rate = read_wav_mono16(Path(args.captured_wav))
    duration_s = len(samples) / sample_rate if sample_rate else 0
    print(f"{args.captured_wav}: {len(samples)} samples @ {sample_rate}Hz ({duration_s:.1f}s)")

    results, all_ok = analyze(samples, sample_rate, ref, search_window_s=args.search_window)
    if not results:
        print("FAIL: could not find the sync tone anywhere in the first few seconds -- "
              "capture doesn't contain this clip, or is silent/misaligned")
        return 1

    n_dropout = 0
    for r in results:
        status = "OK  " if r["ok"] else "DROP"
        if not r["ok"]:
            n_dropout += 1
        print(f"  [{status}] {r['label']:14s} {r['freq_hz']:6.0f} Hz  "
              f"@ {r['expected_start_ms']:6d} ms   rms={r['rms']:6.0f}  "
              f"(floor={r['noise_floor_rms']:.0f})")

    print(f"\n{len(results)} segments checked, {n_dropout} flagged as dropped/missing")
    if all_ok:
        print("PASS: no dropouts detected")
        return 0
    print("FAIL: one or more segments look dropped -- see [DROP] rows above")
    return 1


if __name__ == "__main__":
    sys.exit(main())
