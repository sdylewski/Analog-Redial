#!/usr/bin/env python3
"""Tests for redial_tts.py's audio post-processing (no model needed; needs numpy).

    python3 -m unittest test_redial_tts.py
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import numpy as np
    import redial_tts as tts
except ImportError:  # numpy only lives in the TTS service's own environment
    np = None


@unittest.skipIf(np is None, "numpy not installed")
class TtsAudioTest(unittest.TestCase):
    def test_mulaw_reference_points(self):
        codes = tts.mulaw_encode(np.array([0.0, 1.0, -1.0]))
        self.assertEqual(list(codes), [0xFF, 0x80, 0x00])

    def test_phone_wav(self):
        rate = 24000
        t = np.arange(rate) / rate
        tone = 0.3 * np.sin(2 * np.pi * 440 * t)
        x = np.concatenate([np.zeros(rate // 2), tone, np.zeros(rate // 2)])   # 0.5 s silence each side
        wav = tts.to_phone_wav(x, rate)
        self.assertEqual(wav[:4] + wav[8:16], b"RIFFWAVEfmt ")
        fmt, ch, sr, _, align, bits = struct.unpack("<HHIIHH", wav[20:36])
        self.assertEqual((fmt, ch, sr, align, bits), (7, 1, 8000, 1, 8))
        n = struct.unpack("<I", wav[wav.index(b"data") + 4:][:4])[0]
        self.assertAlmostEqual(n / 8000, 1.08, delta=0.03)   # the tone plus 40 ms margins
        self.assertEqual(struct.unpack("<I", wav[4:8])[0], len(wav) - 8)

    def test_silence_is_an_error(self):
        with self.assertRaises(ValueError):
            tts.to_phone_wav(np.zeros(24000), 24000)

    def test_clean_text(self):
        self.assertEqual(tts.clean_text("  Hi\x00 there\n\n now "), "Hi there now")


if __name__ == "__main__":
    unittest.main()
