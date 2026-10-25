# Low-power idle mode — background

Implements the optional "Low-power idle mode" checkbox on Settings ->
General (`idle_power` component). This is the engineering-journal writeup:
where the numbers came from, what was deliberately left out of the first
pass, and open questions for later. The user-facing description lives only
on the Settings page itself (deliberately short — see "UI copy" below).

## The question this started from

The board sits idle almost all its life. Two independent, physical power
draws exist even with nobody on the phone: the AG1171 SLIC(s), which supply
constant loop current to sense a lifted handset, and the ESP32-C6 with WiFi
kept fully awake (`WIFI_PS_NONE` — see `wifi.c`, and "Why WiFi is
untouched by default" below). Is there a way to cut both without losing (a)
instant off-hook detection or (b) VoIP call delivery?

## Datasheet numbers (all already in `datasheets/`)

**AG1171 SLIC**, §8.3 DC Electrical Characteristics, @5.0V (this board's SLIC
rail — `docs/hardware-smd4.md` §2):

| State | Current | |
|---|---|---|
| On-hook | 60mA typ / 90mA max | |
| Ringing | 330mA typ / 480mA max | |
| Off-hook (active) | 290mA typ / 400mA max | |
| **Power-down (PD low)** | **7mA typ** | |
| Wake-up time | 50ms | "logic outputs not valid" before this |
| SHK debounce | 10ms | |

§3.4 is explicit that **SHK does not work while powered down** — there's no
loop current to sense a handset with, since the SLIC's own DC/DC converter
is what supplies it. The datasheet's own recommendation for power-down use
is to poll (power up periodically, check SHK, power back down) — that's
exactly what this feature does.

**ESP32-C6-WROOM-1**, §6.4:
- WiFi RX-active (closest proxy for "associated, `WIFI_PS_NONE`, otherwise
  idle" — the datasheet doesn't characterize that exact state): ~78-82mA
  @3.3V.
- Modem-sleep (`WIFI_PS_MIN_MODEM`, radio clock-gated between DTIM beacons):
  14-38mA typ depending on CPU load/frequency.
- Deep-sleep: ~7µA, but wakes via a full reboot (WiFi reconnect included) —
  see "Why not deep-sleep" below.

**ES8388 codec**, §8.5: 7-59mW depending on rail/mode. Small enough next to
the above that it's not worth optimizing in this pass.

## Design

One user-facing setting: a checkbox + "minutes idle" field (NVS-backed,
`idle_power_get/set()`, mirrors `board_offhook_alert_get/set()`'s pattern).
Internally, two tiers, chosen automatically — no VoIP-related UI shown to
the user, per their explicit request; this doc is the record of *why*:

1. **Always, once idle past the threshold:** duty-cycle each on-hook line's
   `ag1170_set_power()` — 100ms on / 500ms period (20% duty; widened from
   an original 250ms period after real-world feedback that a handset takes
   ~500ms-1s to reach your ear after being lifted, so the original latency
   budget had room to spend). Average current: `0.2*60 + 0.8*7 = 17.6mA`,
   vs. 60mA continuous — about **71% savings per line** (~0.21W/line at
   5V). See "Timing derivation" below for where 100ms/500ms come from.
2. **Additionally, WiFi power-save** (`wifi_set_power_save(true)` ->
   `WIFI_PS_MIN_MODEM`), with or without a VoIP server or board-to-board
   numbers (changed 2026-10-25; it used to be skipped whenever a VoIP server
   was configured). The radio sleeps between router beacons and wakes for each
   DTIM beacon, where the access point hands over anything it buffered for it.
   So an incoming call's first packet (a SIP INVITE) waits at most one DTIM
   interval, typically 0.1-0.3 s (a router set to DTIM 10 would make it about
   1 s). The bell starts the instant the packet arrives, and the radio goes
   fully awake the moment any line leaves idle (ringing counts), well before
   the call's audio. Outgoing packets (the registration refresh, a call being
   placed) wake the radio themselves. A call can't be missed outright: the
   access point holds frames for a sleeping station.

Both tiers disarm instantly (`wake_fully()`) the moment any line leaves
`TEL_IDLE` — off-hook, a call, dialing, recording, ringing, all of it.
`idle_power.c` polls `telephony_state()` for this; it does not hook into
telephony.c at all (zero changes to that component), keeping the
integration read-only and low-risk against the most safety-critical part
of the codebase.

### Why WiFi is untouched by default

`wifi.c`'s `wifi_init()` has its own comment explaining that
`WIFI_PS_MIN_MODEM` was tried and reverted: "~100ms+ HTTP latency and
intermittent 'no route to host' dropouts under load — bad for a device
whose whole job is to serve a web UI and carry VoIP RTP." That's a real,
previously-measured regression on this exact hardware, not a hypothetical
— hence gating the WiFi tier specifically on "no VoIP," where the downside
degrades to "web UI feels slightly slower right after a long idle stretch"
instead of "a live call's audio glitches." A board with VoIP configured
never touches WiFi power-save in this feature, full stop.

### Why not deep-sleep

Deep-sleep wakes by rebooting from scratch, WiFi reconnect included —
hundreds of ms to seconds. That's far too slow to be the thing polling SHK
every 250ms; the mechanism that actually works for that is **light-sleep**
(fast wake, RAM/state preserved). This pass doesn't even use light-sleep,
though — see "What's deliberately deferred" below. The other reason
deep-sleep specifically doesn't fit: if the SLIC is *also* powered down at
that moment, there's no loop current for SHK to reflect regardless of what
watches the wake GPIO. Deep-sleep is the wrong tool for a device that needs
to check something every fraction of a second, VoIP configured or not.

### Timing derivation (100ms on / 500ms period)

AG1171 wake-up time (50ms) + SHK debounce (10ms) + margin ≈ 100ms needed
before a genuine off-hook reading during an "on" window can be trusted. Once
trusted, `ag1170.c`'s existing, already-proven hook/pulse state machine
takes over exactly as it does in normal full-power operation (this feature
doesn't reimplement hook detection — it only decides when PD is high) —
including its own `offhook_confirm_ms` (80ms) debounce, which happens either
way and isn't "extra" latency from this feature.

Eco-mode-*specific* added latency = (poll interval − on-time), waiting for
the next window, + the ~100ms settle inside that window. With
interval=500ms, on-time=100ms: worst case ≈ 400 + 100 = 500ms, plus the
baseline 80ms confirm ≈ **~580ms total**, which is where the "~600ms" figure
quoted on the Settings page comes from. This was **not bench-measured** —
it's derived from the datasheet's own numbers plus this codebase's existing
confirm-timer constant, and the choice to widen the period from 250ms to
500ms was based on the user's own sense of real handset-to-ear travel time
(~500ms-1s), not a bench measurement either. Worth confirming both with a
stopwatch/power meter once real hardware can be tested.

## What's deliberately deferred (not implemented in this pass)

- **ESP32 CPU light-sleep / `esp_pm` dynamic frequency scaling.**
  `CONFIG_PM_ENABLE` is not even on in `sdkconfig` right now. Turning it on
  is a much bigger change than a single WiFi API call — it affects FreeRTOS
  tickless idle scheduling for every task, not just this feature's. Given
  the WiFi power-save regression this project already hit once, a second
  system-wide power-management change felt like too much blast radius for
  one pass. `wifi_set_power_save()` (a single, purpose-built,
  self-contained API) gets most of the same WiFi-side savings without it.
  Worth a dedicated follow-up if the current numbers aren't enough.
- ~~**Resetting the idle timer on web UI activity.**~~ **Implemented**
  (2026-09-08) — this was in fact annoying in practice almost immediately:
  WiFi power-save engaged mid-Settings-browsing on a real board (no VoIP
  configured), reproducing the exact `WIFI_PS_MIN_MODEM` HTTP-latency
  regression `wifi.c`'s own comment warns about, badly enough that pages
  looked "stuck." Fix didn't need wiring activity tracking through ~40
  route handlers as feared — `esp_http_server`'s `httpd_config_t.open_fn`
  hook fires once per new connection regardless of which route it's for,
  so `webui_start()` sets that to a one-line function calling the new
  `idle_power_note_activity()`, which restarts the idle clock and
  immediately restores full power (SLIC + WiFi) if already sleeping, on
  `idle_power_task()`'s own next tick (≤ `TICK_MS`). One new function,
  one config field, zero per-handler changes.
- **Per-line independent idle timers.** One global timer covers all lines;
  a 2-line board with one line mid-call keeps *both* fully awake (the "all
  lines idle" check in `all_lines_idle()`), even though only the busy line
  strictly needs it. Simpler and safer; splitting it out is possible later
  if it matters.

## Estimated savings shown in the UI

`idle_power_estimated_savings_w()` computes `0.21W * num_lines`, plus
`0.18W` if no VoIP server is configured — both constants straight from the
"Design" section's math above. This is a datasheet-derived estimate, not a
bench measurement on real hardware. If someone puts a USB power meter on a
DevKit and measures the real before/after idle draw, update the constants
in `idle_power.c` (`SLIC_SAVINGS_W_PER_LINE`, `WIFI_SAVINGS_W`) to match
reality rather than the datasheet estimate.

## Open questions for later

- Real bench measurement of both the savings number and the ~600ms latency
  claim, on actual hardware.
- 100ms/500ms (widened once already, based on user feedback rather than a
  bench measurement) may still not be the final answer — whether an even
  longer period is worth offering as a second "aggressive" checkbox once
  this version has been lived with a while.
- Whether `esp_pm`/light-sleep is worth revisiting once there's a stronger
  case for the extra savings.

## Adaptive Wi-Fi transmit power (removed 2026-10-22)

The firmware used to lower the Wi-Fi TX power from 20 to 11 dBm when the received signal was strong,
on the assumption that the link is symmetric. On a mesh network it isn't: the board dropped off the
router (disconnect reason 4, inactivity) and pages stalled, only while reduced. Every failure on one SMD2
board fell inside a reduced-power period; back at 20 dBm the send errors stopped. The saving was also
small: about 1-3 mA average on an idle web server (the radio transmits a few percent of the time), against
about 40 mA per line for SLIC power-down and about 50 mA for Wi-Fi modem sleep. So the feature, its setting
and its web UI row were deleted. The radio now always runs at full power.
