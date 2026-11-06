/*
 * line_expander.h -- line 1's SLIC control pins on a TCA9534A I2C expander.
 *
 * Analog Redial v6 (BOARD_LINE1_ON_EXPANDER) moves line 1's F/R, RM, SHK and
 * PD off GPIO18-21, which are the microSD card's on that board. ag1170.c
 * calls these instead of the gpio driver for that one line. They never touch
 * I2C themselves: outputs and the SHK reading are cached here and a small task
 * does the bus traffic, so the 1 ms monitor task can't be held up by the bus
 * and line 0's timing doesn't change.
 *
 * Written before any v6 board exists: not yet tested on hardware.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the chip to the safe idle (not ringing, SLIC running, spare pins low)
 * and starts the task. A chip that doesn't answer isn't fatal: the task keeps
 * retrying, and the line reads on-hook until it does. A second call does
 * nothing: ag1170 calls it for a second line, auxpins for J10's pins. */
esp_err_t line_exp_init(void);

void line_exp_set_fr(int level);
void line_exp_set_rm(int level);
void line_exp_set_pd_run(bool run);
/* Latest SHK level read over I2C, at most about 4 ms old. */
int  line_exp_shk(void);

/* J10's spare pins, P4-P7 (n = 0-3): outputs, low from power-up until set.
 * Safe from any task or timer callback; the task sends it within a poll.
 * line_exp_spare() is the level last asked for. */
#define LINE_EXP_SPARE_N 4
void line_exp_set_spare(int n, int level);
int  line_exp_spare(int n);
/* line_exp_init() has run (it's safe to call again). */
bool line_exp_started(void);

typedef struct {
    bool     chip_ok;     /* answering, and its registers match what was written */
    uint8_t  input;       /* last input register read */
    uint8_t  output;      /* output register as last written */
    uint8_t  config;      /* configuration register as last written */
    uint32_t bus_errors;  /* failed transfers since boot */
    uint32_t chip_resets; /* times the chip was found back at its power-on state */
} line_exp_status_t;

void line_exp_status(line_exp_status_t *out);

#ifdef __cplusplus
}
#endif
