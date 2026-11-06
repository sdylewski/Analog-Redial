# Payphone features in the firmware

Settings → **Payphone**, marked *Experimental*. Everything on it works on any
Analog Redial board, without the coin board. Items marked **[verify]** are from
memory and need checking against a real phone. The coin board's part (coin
switches, collecting and returning coins) isn't built yet: see the end.

## The tab

- **Enable Payphone** (off by default). Off, the board behaves exactly as before
  and nothing below runs; the settings are kept.
- **Coin required.** Lifting the handset plays a spoken message ("Please deposit
  10 cents for your call", text editable) instead of dial tone, repeated every few
  seconds, until the coins add up to the **price**. Then dial tone. Calls (the other
  line, VoIP, Bluetooth, other boards) always need the credit and use it up when
  placed. With **"the board's own dial codes are free"** ticked (the default), the
  operator, clips, timer and so on work without a coin, even before dial tone; untick
  it and every code waits for a coin. Dialing a call without enough credit goes back
  to the message. Credit is cleared when the handset goes down. Left waiting longer
  than the off-hook alert's delay (Settings → Phone), the line sounds the alert.
- **Message after a coin.** Off by default. Each coin can play its own clip (any
  audio file on the board, picked by name), or the general message ("Thank you",
  text editable). It plays while the line is waiting for a coin or on dial tone,
  never during dialing or a call.
- **Coin tones.** Listens to the line for the tones a payphone's totalizer sends:
  1700 and 2200 Hz together, one 66 ms beep for a nickel, two for a dime, five 33 ms
  beeps for a quarter **[verify]**. It counts any beep and gap of 20 ms or more, so
  timing a little off still works; a burst it can't place is logged as not
  recognized. Whether a converted phone still sends these depends on the phone:
  the totalizer needs line current, and some conversions bypass the coin mechanism
  **[verify on real phones]**.
- **Coins.** The credit on each line, a coin box counter (nickels, dimes, quarters,
  total) with a reset, **simulate** buttons for each coin on each line (handset up),
  and a log of the last 40 events: coins with their source and the credit after,
  calls that needed more, credit used, unrecognized tones. The log refreshes by
  itself every 2 seconds and is lost on a restart; the counter is kept.

The two spoken messages are prompts like the operator message: type the text, then
generate the audio with `gen_prompt.py --sync` or the VoIP server's text-to-speech
(the tab shows the exact command). Until the "please deposit" audio exists, the line
is silent while waiting for a coin.

## For scripts and Home Assistant

- `GET /api/payphone`: settings, credit per line, the counter and the log, as JSON.
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
- `webui.c`: the tab, `/payphone_enable`, `/payphone_set`, `/payphone_coin`,
  `/payphone_reset`, `/api/payphone`.

## Still to build

- **The coin board** (this folder): a driver for U1 (I2C 0x39, coin inputs P0-P3,
  polled every 2-5 ms and debounced, coin-board-schematic.md section 8) and U4, coins
  from its switches as a third source, and collect / return: collect when a call is
  answered, return when the caller hangs up first or the call fails, with test buttons,
  the firing sequence and limits of coin-board-schematic.md section 7. (Low-voltage
  coils need no firmware: they use the board with its J14 jumper off.)
- Timed calls ("please deposit 5 cents for the next 3 minutes").
- A value for coin 4 (50 c, $1, or "coin present").
- A refund of the credit when a call fails, instead of using it when the call is placed.
