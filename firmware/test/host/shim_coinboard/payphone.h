#pragma once
/* The parts of payphone.h coinboard.c uses. */
#include <stdbool.h>
typedef enum { PAYPHONE_SRC_TONE = 0, PAYPHONE_SRC_SIM, PAYPHONE_SRC_BOARD } payphone_src_t;
typedef enum { PAYPHONE_COLLECT = 0, PAYPHONE_RETURN = 1 } payphone_relay_t;
typedef void (*payphone_relay_fn)(int line, payphone_relay_t what);
void payphone_set_relay_handler(payphone_relay_fn fn);
void payphone_set_board_line(int line);
bool payphone_enabled(void);
void payphone_logf(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
