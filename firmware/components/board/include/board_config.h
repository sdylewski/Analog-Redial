#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exact PCB this firmware was built for, for display strings (Settings ->
 * Hardware, /api/status) and flash_board.py's board detection. */
static inline const char *board_pcb_variant_name(void)
{
#if CONFIG_BOARD_TYPE_SMD2
    return "SMD2";
#elif CONFIG_BOARD_TYPE_SMD4
    return "SMD4";
#elif CONFIG_BOARD_TYPE_SMD5
    return "SMD5";
#elif CONFIG_BOARD_TYPE_SMD5_CHEAP
    return "SMD5 (clone module)";
#else
#error "Unknown CONFIG_BOARD_TYPE_* -- pick a board in menuconfig"
#endif
}

/* Which firmware build this is, for refusing a browser upload meant for a
 * different board (wrong pin map) or flash size (wrong storage limits):
 * "<board>/<flash size>", e.g. "smd4/16MB". */
#if CONFIG_BOARD_TYPE_SMD2
#define BOARD_IMAGE_TYPE "smd2"
#elif CONFIG_BOARD_TYPE_SMD4
#define BOARD_IMAGE_TYPE "smd4"
#elif CONFIG_BOARD_TYPE_SMD5
#define BOARD_IMAGE_TYPE "smd5"
#elif CONFIG_BOARD_TYPE_SMD5_CHEAP
#define BOARD_IMAGE_TYPE "smd5_cheap"
#endif
#define BOARD_IMAGE_ID BOARD_IMAGE_TYPE "/" CONFIG_ESPTOOLPY_FLASHSIZE

/* The same id embedded in the firmware image, framed so it can be found in
 * an uploaded .bin: "\x01AR-BOARD\x02" BOARD_IMAGE_ID "\x03". Defined in
 * board.c; search for it using its own bytes (never a second literal of the
 * prefix, which would also end up in the image and match first). */
extern const char board_image_marker[];

#ifdef __cplusplus
}
#endif
