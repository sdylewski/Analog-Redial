/*
 * g711.h — G.711 µ-law (PCMU, RTP payload type 0) codec, inline.
 *
 * The North-American / Asterisk default. 8 kHz, 8-bit, 2:1 vs. 16-bit PCM.
 * Standard 16-bit linear <-> µ-law conversion (bias 0x84, clip 32635).
 */
#pragma once

#include <stdint.h>

static inline uint8_t ulaw_encode(int16_t pcm)
{
    const int16_t BIAS = 0x84;
    const int16_t CLIP = 32635;
    int sign = (pcm >> 8) & 0x80;
    if (sign) pcm = (int16_t)-pcm;
    if (pcm > CLIP) pcm = CLIP;
    pcm = (int16_t)(pcm + BIAS);
    int exponent = 7;
    for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; exponent--, mask >>= 1) { }
    int mantissa = (pcm >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

static inline int16_t ulaw_decode(uint8_t u)
{
    const int BIAS = 0x84;
    u = (uint8_t)~u;
    int sign = u & 0x80;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0F;
    int sample = ((mantissa << 3) + BIAS) << exponent;
    sample -= BIAS;
    return (int16_t)(sign ? -sample : sample);
}
