/*
 * bringup.h — interactive serial console for board bring-up.
 *
 * Registers esp_console commands that exercise each IC and subsystem
 * step-by-step. Safe to run with no PCB attached (hardware-dependent commands
 * just report failures). See firmware/README.md for the bring-up checklist.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Register all bring-up console commands, then start the REPL. */
void bringup_console_start(void);

#ifdef __cplusplus
}
#endif
