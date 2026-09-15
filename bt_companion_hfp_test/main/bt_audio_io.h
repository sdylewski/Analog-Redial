/* bt_audio_io.h — bridges HFP's vHCI audio samples to the analog GPIO25
 * (DAC, downlink) / GPIO34 (ADC, uplink) pins, per
 * archive/BLUETOOTH_AUDIO_UPGRADE.md's analog-handoff plan. HFP over CVSD
 * is 16-bit signed PCM, mono, 8kHz -- both directions run at that rate to
 * match the phone-call codec, regardless of the DAC/ADC's own native
 * resolution (8-bit unsigned, converted here).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the DAC continuous channel (GPIO25) and the ADC continuous
 * channel + background sampling task (GPIO34). Call once, before the first
 * HFP audio connection -- cheap to leave running idle between calls. */
esp_err_t bt_audio_io_init(void);

/* Converts `len` bytes (as 16-bit signed PCM, so len should be even) and
 * writes them out the DAC (GPIO25) -- call this from
 * bt_app_hf_client_incoming_cb() with exactly what it received. Blocks
 * until the DMA has room, same as a direct dac_continuous_write() would. */
void bt_audio_io_write_downlink(const uint8_t *pcm16, size_t len);

/* Fills up to `max_len` bytes (16-bit signed PCM) from the ADC's own
 * background-sampled ring buffer -- call this from
 * bt_app_hf_client_outgoing_cb() instead of the stock loopback. Returns
 * the number of bytes actually filled (0 if the ring buffer's run dry,
 * matching the "not enough data" contract the outgoing callback expects). */
uint32_t bt_audio_io_read_uplink(uint8_t *out, uint32_t max_len);

/* Diagnostic: synthesizes a sine wave and feeds it through the exact same
 * bt_audio_io_write_downlink() path real HFP audio uses, entirely bypassing
 * Bluetooth/the phone -- lets a recording taken on the analog line (the C6's
 * `record`/`/test/record`) isolate whether the DAC -> analog bridge -> C6
 * ADC chain itself is clean, independent of whatever the live call audio
 * looks like. Runs in its own background task; returns immediately. */
void bt_audio_io_test_tone(int hz, int ms);

#ifdef __cplusplus
}
#endif
