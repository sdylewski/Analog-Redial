#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_TYPE_THT = 0,
    BOARD_TYPE_SMD = 1,
} board_type_t;

static inline board_type_t board_current_type(void)
{
#if CONFIG_BOARD_TYPE_SMD2 || CONFIG_BOARD_TYPE_SMD4 || CONFIG_BOARD_TYPE_SMD5 || CONFIG_BOARD_TYPE_SMD5_CHEAP
    return BOARD_TYPE_SMD;
#else
    return BOARD_TYPE_THT;
#endif
}

static inline const char *board_type_name(board_type_t type)
{
    return (type == BOARD_TYPE_SMD) ? "smd" : "tht";
}

/* board_current_type() deliberately collapses SMD2 and SMD4 into one
 * BOARD_TYPE_SMD -- they share the line2/microSD GPIO tradeoff and every
 * other codepath that branches on board TYPE only cares about that shared
 * behavior, not which specific PCB revision it's running on. Display
 * strings (Settings -> Hardware, /api/status) need the precise PCB name
 * though -- this is that, kept separate on purpose rather than adding a
 * 3rd board_type_t value that most callers would have to special-case
 * right back down to "it's still SMD" anyway. */
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
    return "through-hole";
#endif
}

#ifdef __cplusplus
}
#endif
