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

#ifdef __cplusplus
}
#endif
