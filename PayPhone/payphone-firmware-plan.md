# Payphone features in the firmware

Settings → **Payphone**, marked *Experimental*. Everything on it except the Coin
board card works on any Analog Redial board, without the coin board. Items marked
**[verify]** are from memory and need checking against a real phone. The coin
board's driver is written and passes its tests against simulated chips, but no
coin board has been built, so none of it has run on real hardware.

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

## Collect and return

The firmware keeps the coins dropped since the handset was lifted (the phone's hopper
holds them):

- **A call this line placed is answered** (ringback or dialing, then connected): the
  coins are collected. Coins dropped during the call are collected at hang-up.
- **The handset goes down with coins and no answered call** (a busy line, no answer, only
  the board's own free codes): they're returned.
- **A coin from the coin board while the handset is down** is returned at once.

Each one goes in the log ("25 c collected: call answered"), with "no coin board on
this line" when there's nothing to fire, so the logic can be watched with the
simulate buttons on any board.

## The coin board (`firmware/components/coinboard/`)

The **Coin board** card: "A coin board is connected", which line it's on, automatic
collect and return, a swap for a relay wired the other way round, the FIRE pulse
(100 ms by default, 20-1000 ms), and what coin 4 is worth (0: not a coin, only logged).
With it set up, the card shows the board live: U1 and U4 answering, each coin switch,
the 20 V rail, HV, the coil current, AIN3, and the relay's state. Test buttons
collect, return, or hold HV for 10 s without firing, to read the voltage. The
console has the same as `coinboard [collect|return|hv]`, and the self-test checks the
board when it's set up.

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
features switched off).

**Bringing up the first board** (J14 off, so HV can't pass about 19 V):

1. Cable to the main board, then the card: "A coin board is connected". The card should
   say U1 and U4 answer, and show the 20 V rail at about 4.6 V idle. Run `coinboard` on
   the console for the registers.
2. Close each coin switch by hand: the card shows it closed and the log gets one coin.
   Drop real coins and check for doubles or misses.
3. **HV on** with no relay: the rail reads about 20 V and HV about 19 V; check both with a
   meter. Then Collect and Return with no relay: "no current flowed".
4. A 12-24 V coil or a resistor (100 ohm, 5 W) on J11: Collect and Return should read a
   current; compare with a meter.
5. Fit J14 (52 V) and the HD550002C: HV test, then Collect and Return. Adjust the pulse.

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
- `webui.c`: the tab, `/payphone_enable`, `/payphone_set`, `/payphone_coin`,
  `/payphone_reset`, `/coinboard_set`, `/coinboard_test`, `/api/payphone`.
- `coinboard/`: the coin board (above). `telephony.c` keeps each line's hopper and
  calls `payphone_relay()`; the coin board registers as its handler.

## Still to build

- Running all of the above on a real coin board (bring-up steps above).
- Timed calls ("please deposit 5 cents for the next 3 minutes").
- A refund of the credit when a call fails, instead of using it when the call is placed.
- Optional: an "HV at least" setting, so a board with stage 2 broken stops with a fault
  instead of firing at 19 V.
