# BM64 MFB/RST_N/P2_0 GPIO Debug (2026-09-24/25)

## Symptom

Bench-wired BM64 control pins (MFB/RST_N/P2_0 on GPIO18/19/20, see
`board_pins_smd4.h`) intermittently failed to bring up BM64's UART at all --
`bm64_test`/`bm64_pin` reported success (`gpio_set_level()` returning
`ESP_OK`), but the module either produced no valid frames, garbage, or (once
MFB was hand-tied straight to the 3.3V rail instead of a GPIO) worked
perfectly every time. This looked exactly like a wiring/connection fault --
long, skinny bench wires; a suspect ground point; different physical pins
tried (GPIO18, then GPIO21) -- and cost most of two evenings chasing it as
one.

## False leads (ruled out, in order tried)

1. **Bad physical connection** -- reseated wires, moved ground to a
   known-good header, swapped MFB between GPIO18/GPIO21, power-cycled.
   Continuity measured at 0.2 ohms end to end. None of this fixed it.
2. **MFB needs a rising edge, not a static level** -- the real datasheet
   names this pin "UART RX_IND, active-high (used by host MCU to wakeup the
   Bluetooth system)", suggesting an edge-triggered wake. A genuine
   low-then-high pulse via GPIO was tested and still failed intermittently
   -- ruled out as the *sole* explanation, though the pulse is harmless and
   was kept.
3. **Idle-power / light-sleep GPIO isolation** -- this project's
   `idle_power` component (`components/idle_power/idle_power.c`) enables
   WiFi power-save after N minutes of an idle phone line, and ESP-IDF's own
   boot log ("sleep: Configure to isolate all GPIO pins in sleep state")
   floats GPIOs during real light-sleep windows. Disabling idle-power mode
   (Settings -> Hardware) did NOT fix the symptom -- ruled out as the cause,
   though it's still a real, independently worth-knowing gotcha (see
   CLAUDE.md's "Bench-debugging GPIOs" section) for *future* sessions,
   since it can legitimately make a bench-wired pin flaky over a long enough
   idle window even though it wasn't the culprit here.
4. **SleepSelEn left set** -- `gpio_dump_io_configuration()` (ESP-IDF's own
   pin-state dump, `driver/gpio.h`) showed `SleepSelEn: 1` on GPIO18.
   Disabling it with `gpio_sleep_sel_dis()` changed the dump's output but
   did NOT fix the readback -- ruled out as the cause. Kept anyway (cheap,
   and a real guard against pin state changing if this board ever does
   enter real light sleep some other way).

## Actual root cause

`bm64_pin` (bringup.c) and `assert_bm64_control_pins()` (bt_companion.c)
configured these pins with a bare `gpio_set_direction(pin, GPIO_MODE_OUTPUT)`
followed by `gpio_set_level()`. That call does not touch `pull_up_en` --
whatever it was left at (including a default/leftover internal pull-up)
stays in effect and can fight the output driver. A `gpio_dump_io_configuration()`
capture caught this directly: `Pullup: 1` sitting right next to
`OutputEn: 1` on a pin that measured 0V both via software readback and a
multimeter on the ESP32's own pad -- not just downstream at BM64.

This project's other bench-configured GPIO outputs (`ag1170.c`'s
`config_line_gpio()`, driving the SLIC's FR/RM pins) never hit this because
they always use the full `gpio_config()` struct, which sets `pull_up_en`
and `pull_down_en` explicitly in the same atomic call as the mode. Switching
`bm64_pin`/`assert_bm64_control_pins()` to the same pattern fixed the issue
completely and reproducibly -- full valid multi-frame BM64 replies over
pure GPIO control, no hand-tied 3.3V wire needed.

## Lesson

**Always configure a GPIO output with the full `gpio_config()` struct
(explicit `pull_up_en`/`pull_down_en`/`intr_type`), not a bare
`gpio_set_direction()` + `gpio_set_level()` pair.** The latter can silently
leave a pull resistor enabled from a prior configuration (including
ESP-IDF's own boot-time defaults, or an earlier `gpio_reset_pin()` call,
which resets pins to their power-on default -- which includes an enabled
pull-up on many pins), and neither `gpio_set_level()`'s return value nor
(on this chip, with `GPIO_MODE_OUTPUT` leaving `InputEn: 0`) a
`gpio_get_level()` readback will reveal the problem -- both report success.
`gpio_dump_io_configuration()` is the tool that actually shows what's
happening at the pad; reach for it early next time, not after several hours
of wiring-focused debugging.
