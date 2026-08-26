/*
 * MP3 decode support. Vendors minimp3 (CC0, third_party/minimp3/), a small
 * single-header decoder, rather than a heavier one like Helix. Only
 * compiled in when CONFIG_AUDIO_CONVERT_MP3 (webui.c upload) or
 * CONFIG_PLAYER_MP3 (player.c playback) is on (see CMakeLists.txt), so its
 * flash cost can be measured directly by toggling both off and comparing
 * .bin size.
 */
#include "audio_convert.h"

#if defined(CONFIG_AUDIO_CONVERT_MP3) || defined(CONFIG_PLAYER_MP3)

#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"

struct audio_mp3_decoder {
    mp3dec_t dec;
};

audio_mp3_decoder_t *audio_mp3_decoder_create(void)
{
    audio_mp3_decoder_t *d = malloc(sizeof(*d));
    if (!d) return NULL;
    mp3dec_init(&d->dec);
    return d;
}

void audio_mp3_decoder_destroy(audio_mp3_decoder_t *dec)
{
    free(dec);
}

bool audio_convert_mp3_magic(const uint8_t *buf, size_t len)
{
    if (len >= 3 && memcmp(buf, "ID3", 3) == 0) return true;
    /* MPEG frame sync: 11 set bits (0xFF followed by top 3 bits set). */
    if (len >= 2 && buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0) return true;
    return false;
}

int audio_mp3_decode_frame(audio_mp3_decoder_t *dec, const uint8_t *in, size_t in_len,
                            int16_t *out, int *out_channels, uint32_t *out_rate,
                            size_t *consumed)
{
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&dec->dec, in, (int)in_len, out, &info);
    *consumed = (size_t)info.frame_bytes;
    if (out_channels) *out_channels = info.channels;
    if (out_rate) *out_rate = (uint32_t)info.hz;
    return samples;
}

#endif /* CONFIG_AUDIO_CONVERT_MP3 || CONFIG_PLAYER_MP3 */
