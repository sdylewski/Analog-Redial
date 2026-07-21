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
#if CONFIG_BOARD_TYPE_SMD2
    return BOARD_TYPE_SMD;
#else
    return BOARD_TYPE_THT;
#endif
}

static inline const char *board_type_name(board_type_t type)
{
    return (type == BOARD_TYPE_SMD) ? "smd" : "tht";
}

#ifdef __cplusplus
}
#endif
