# Payphone features in the firmware

Settings → **Payphone**, marked *Experimental*. Everything on it except the coin
board works on any Analog Redial board. Items marked **[verify]** are from memory and
need checking against a real phone. The coin board and the whole tab run the
project's first payphone (GTE / Quadrum style, coin board v1 on a v6 board, since
2026-11-15); coin tones have not been heard from a real phone yet. What each phone
model needs is collected in [the payphone types](../README.md#payphone-types).

The tab has three panes, `?sub=settings` (the default; `sub=all` shows every card),
`log` and `bringup`, each building only its own cards.

## Phone type

The first card on the Settings pane. Two menus, in order:

1. **Housing**: Quadrum / GTE 120B style, Western Electric / AT&T single-slot,
   Western Electric 3-slot, Nortel Millennium, Other or not sure (with the years each
   was made).
2. **What handles the coins**, listing only what that housing is found with, the
   usual first: coin switches and relay wired to the coin board, **no coin board (use
   it as a phone, listening for any coin tones it sends)**, its own smart board, a
   coin-line totalizer (coin tones), coinless, or not sure. Every choice but the coin
   board one is labelled **untested** in the menu and in its status row, as is the Coin
   tones card, until a real phone has shown it works (`tested` in `INSIDES[]`).

Below them, a description of each choice as it's picked, and **A coin board is
installed** (with a link to the board's page) when the choice uses one. **Use these
settings** saves both, sets coin tones and coin required as that choice needs, turns
the coin board on or off from the box, and for a measured phone sets the coin
board's pulse (and swap, for a measured phone whose swap is known). The saved choice then shows a table: dialing, ringing, how
coins are seen, how the relay is worked, the coin board, and the measured values
(with the shunts to fit, which firmware can't set).

Cards that don't apply are hidden, with a line naming them and **Show all
settings** (`sub=all`). A smart board or coinless phone hides every card; a coin
board phone hides Coin tones; the Coin board card shows only while a board is
installed (or for "not sure"). The Bring-up pane always shows everything. "Not
sure" changes nothing and shows every card, which is the way to start on a phone
nobody has tried.

The table is `components/payphone/payphone_types.c` (plain data, host-tested by
`test/host/test_payphone_types.c`): `HOUSINGS[]`, `INSIDES[]` and `KNOWN[]`, the
measured phones. The categories and what each needs are in
[the payphone types](../README.md#payphone-types). The choice is stored as two bytes
appended to `payphone_cfg_t`.

## The tab

- **Enable Payphone** (off by default). Off, the board behaves exactly as before
  and nothing below runs; the settings are kept.
- **Coin required.** Lifting the handset plays a spoken message ("Please deposit
  10 cents for your call", text editable) instead of dial tone, repeated every few
  seconds, until the coins add up to the **price**. Then dial tone. Calls (the other
  line, VoIP, Bluetooth, other boards) always need the credit and use it up when
  placed. With **"Every dial code is free"** ticked (the default), the
  operator, clips, timer and so on work without a coin, even before dial tone; untick
  it and every code waits for a coin, except the ones ticked in the **Free** column of
  the Dial Codes tab (shown while payphone features are on; the calls to the other line
  have no tick, as they always cost a coin). Dialing a call without enough credit goes back
  to the message. Credit is cleared when the handset goes down. Left waiting longer
  than the off-hook alert's delay (Settings → Phone), the line sounds the alert.
- **Message after a coin.** Off by default. After the phrase, dial tone comes back after about 0.3 s. A coin's clip starts at once (no lead-in pause), and a coin
  that arrives while one is playing is queued (up to four), so two quarters in a row say the phrase twice,
  the second right after the first. Each coin can play its own clip (any
  audio file on the board, picked by name), or the general message ("Thank you",
  text editable). The defaults are a nickel "Feed me.", a dime "Yum yum." and a
  quarter "Thank you."; each phrase is editable on the card, and
  `firmware/bringup/gen_prompt.py --sync <host>` makes the audio. It plays while the line is waiting for a coin or on dial tone,
  never during dialing or a call.
- **Coin tones.** Listens to the line for the tones a payphone's totalizer sends:
  1700 and 2200 Hz together, one 66 ms beep for a nickel, two for a dime, five 33 ms
  beeps for a quarter **[verify]**. It counts any beep and gap of 20 ms or more, so
  timing a little off still works; a burst it can't place is logged as not
  recognized. Whether a converted phone still sends these depends on the phone:
  the totalizer needs line current, and some conversions bypass the coin mechanism
  **[verify on real phones]**.
- **Coins** (Log pane). The credit on each line, a coin box counter (nickels, dimes, quarters,
  total) with a reset, **simulate** buttons for each coin on each line (handset up),
  and a log of the last 40 events: coins with their source and the credit after,
  calls that needed more, credit used, unrecognized tones. The log refreshes by
  itself every 2 seconds and is lost on a restart; the counter is kept.

The two spoken messages are prompts like the operator message: type the text, then
generate the audio with `gen_prompt.py --sync` or the VoIP server's text-to-speech
(the tab shows the exact command). Until the "please deposit" audio exists, the line
is silent while waiting for a coin.

## Collect and return

The firmware keeps the coins dropped since the handset was lifted (the phone's hopper
holds them):

The **What happens to the coins** card picks when they're collected (tested on the
first phone in all three modes):

| Setting | A call this line placed is answered | The handset goes down |
|---|---|---|
| **Collected when the call is answered** (default, like a real payphone) | the coins are collected | coins dropped during the call are collected; with no answered call, they're returned |
| **Held until the handset goes down** | nothing yet | collected if a call was answered, otherwise returned |
| **Collected as each one drops** | nothing to do: each coin was collected as it dropped | nothing to do |

- A call is "answered" when ringback or dialing turns into a connected call. A busy line,
  no answer, or only the board's own free codes don't count.
- **No answered call:** when the handset goes down with coins and no answered call, they are returned or
  collected, as chosen on the card (the board's own codes, like 314, don't count as an answered call).
- **Coin limit:** the hopper jams if too many coins pile up and are released together, so at a set number
  of coins (8 by default, 0 for no limit) they are returned or collected at once.
- **A coin from the coin board while the handset is down** is returned at once, in every mode.
- Whether the coin board really fires its relay for these is **Bench test mode** on the Bring-up
  pane (on for a board that has never saved it): while it is on, the relay never fires by itself,
  but the log still shows what would have happened.

Each one goes in the log ("25 c collected: call answered"), with "no coin board on
this line" when there's nothing to fire, so the logic can be watched with the
simulate buttons on any board.

## The coin board (`firmware/components/coinboard/`)

The **Coin board** card (Settings pane): "A coin board is connected", which line it's on, a swap for
a relay wired the other way round, the FIRE pulse (100 ms by default, 20-1000 ms), and what coin 4 is
worth (0: not a coin, only logged). The Phone type card sets the first, and for a measured phone the
pulse.

The **Bring-up** pane is for a new board, or a phone nobody has tried:

- **Bench test mode** (on for a board that has never saved it; stored as `!auto_relay`): the relay
  never fires by itself (a coin switch closed with the handset down would otherwise return the coin
  at full HV), but the test buttons always work. Turn it off when the phone is ready.
- **The bring-up checklist**, steps 0-7, each judged live from `/api/payphone`: I2C errors (the 3.3 V
  and ground wires, with a symptom table), U1/U4 answering (and SDA/SCL levels when nothing does),
  the idle rail (the 5 V wire), each coin input (counted, too short, longest closure), the HV test
  against the shunts chosen in a menu (it warns when HV doesn't match), firing into a load with a
  pulse length for that test only, the bleed-down, and the coin relay.
- **The fire plot**: after each collect or return, the coil current every ~1.7 ms through the pulse
  with HV beside it, the peak and when the current fell (the armature's stroke, and the relay's own
  contact opening), and **Copy data** (CSV). The HD550002C's current builds over ~65 ms and dips
  as the armature moves at 65-70 ms.
- **Coin switch capture**: reads the four inputs every ~0.2 ms (400 kHz) and walks through 2-5
  nickels, dimes and quarters, then shows each switch's on-time (min/avg/max), bounce, whole-coin
  length, how many the board would count, and other inputs closed (swapped wires, or a reversed
  plug). Normal polling and the relay are locked out while it runs. `/api/coin_capture`.

The console has `coinboard [collect|return|hv]` and `v6check`, and the self-test checks the board
when it's set up.

Nothing touches the bus for it unless both the Payphone features and "A coin board is
connected" are on.

- **Bus:** U1 (TCA9534A, 0x39) and U4 (ADS1115, 0x48), 100 kHz, shared with the codec
  and, on v6, line 2's expander at 0x38.
- **U1's pins:** P0 coin 4, P1 quarter, P2 dime, P3 nickel (inputs, closed = low);
  P4 FIRE, P5 POL, P6 EN_HV, P7 unconnected (outputs). Configuration register 0x0F.
- **Start-up:** output register to 0 before the configuration register, so no output
  is ever high on the way up. U1 keeps its registers through an ESP32 reset, so this
  runs early in boot (a crash mid-pulse could have left FIRE high), and again on every
  `esp_restart()`.
- **Polling:** U1 every 3 ms. A coin is two closed polls in a row (a one-poll spike is
  noise), then the input has to read open for 30 ms before it counts again, so bounce
  never counts twice. Closed for 2 s: logged once as stuck. Coins go in as a third
  source, "coin board", on the card's line. Coin tones are ignored on that line, since
  the switches see the same coins.
- **Readback:** U1's input register reads every pin, outputs included, so each poll
  also checks that EN_HV, POL, FIRE and P7 are where they were set. Two polls off
  means the chip reset (all inputs: the pulldowns hold everything low) or a pin is held
  by a fault: the sequence stops and the log names the pin. The configuration register
  is also read back once a second.
- **U4:** single conversions at 860 samples a second, ±4.096 V. The coil current while
  firing (with HV every fourth), HV and the 20 V rail while charging, all four slowly
  when idle. Without U4 the sequence uses fixed times and reports "not measured".
- **The sequence** (`coin_fire.c`):
  1. Set POL (low to collect) with HV off. Wait 30 ms for K1.
  2. EN_HV high. Wait until HV stops rising (under 1 V in 250 ms, at least 15 V,
     300 ms to 4 s), or 3 s without U4.
  3. FIRE for the pulse.
  4. FIRE and EN_HV low. POL stays put until the coil current is under 20 mA.
  5. Rest 1.5 s. A second request (collect, then return) waits its turn.
- **It stops early, with the reason in the log, when:**
  - The 20 V rail isn't up after 1 s.
  - HV is under 15 V after 4 s.
  - The current is pinned at full scale (300 mA or more) for 20 ms with HV above 30 V:
    the relay's contact shorting the coil. Not below 30 V, where a 24 V coil draws that
    much by design.
  - Current keeps flowing after FIRE is off (Q3 shorted). POL is then held until it
    stops, so K1 never breaks the current.
  - The bus fails, or the chip resets.
- **The result:** "fired at 59 V, 95 mA peak, HV fell 21 V", "no current flowed (relay
  not connected?)", or the fault.

The firmware can't see J7-J9, so it fires at whatever HV settles at: with stage 2 broken
it fires at about 19 V, as with J14 off.

**Tests:** `firmware/test/host/run.sh` runs `test_coin_inputs.c` (bouncing switches,
every poll phase), `test_coin_fire.c` (the sequence against a model of both power stages
and the relay, checking at every millisecond that FIRE is never high without EN_HV, POL
never changes while current flows, and FIRE never exceeds 1 s; and each fault above),
and `test_coinboard.c` (the driver against simulated U1 and U4: start-up order, coins,
collect and return, a chip reset mid-charge, a held pin, the cable coming off, no U4, the
features switched off). `test_payphone_types.c` checks the phone type table (every housing's insides exist, the
measured phones point at a real pair).

**Bringing up a board** follows the Bring-up pane's steps (the public version is
[hardware-coin-board.md §9](../../docs/hardware-coin-board.md#9-firmware-and-first-power-up)).
On the first board the problems found were, in order: a bad 5 V crimp, A2 not soldered,
VIO at 2.8 V from a clone module's regulator (U4 then reads the rail as 14 V and HV
1.7x high), a bad VIO crimp, a reversed J3 plug, and a stiff coin lever. Start a new relay at
60 V and go up; the HD550002C needed 87 V in the phone.

## For scripts and Home Assistant

- `GET /api/payphone`: settings, credit per line, the counter, the log, and `board`
  (the coin board's status, as on the card), as JSON.
- `POST /payphone_coin` with `line` and `cents` (5, 10 or 25): the same as a
  simulate button.
- Each coin sends the webhook / Home Assistant event `coin`, with the cents as its
  detail.

## How it's built

- `firmware/components/payphone/`: settings and the counter (NVS namespace
  `payphone`), the log, and `coin_tone.c`, the tone detector (two Goertzel bins on
  10 ms windows at 8 kHz; no ESP-IDF code, so `firmware/test/host/test_coin_tone.c`
  runs it against synthetic coins, dial tone, DTMF and noise).
- `telephony.c`: the detector runs on each line's received audio next to the DTMF
  detector, only while off-hook; a coin is a queued event like a digit. The state
  `COIN_WAIT` is the "please deposit" wait; the coin rule sits in front of every
  dial-plan action.
- `webui.c`: the tab, `/payphone_enable`, `/payphone_type`, `/payphone_set`, `/payphone_coin`,
  `/payphone_reset`, `/coinboard_set`, `/coinboard_test`, `/api/payphone`.
- `coinboard/`: the coin board (above). `telephony.c` keeps each line's hopper and
  calls `payphone_relay()`; the coin board registers as its handler.

## Still to build

- Coin board v2: the ground-start relay on P7 (for coin-first coin-line phones), and its
  firmware.
- A second phone type measured, so `KNOWN[]` has more than one entry.
- Timed calls ("please deposit 5 cents for the next 3 minutes").
- A refund of the credit when a call fails, instead of using it when the call is placed.
- Optional: an "HV at least" setting, so a board with stage 2 broken stops with a fault
  instead of firing at 19 V.
