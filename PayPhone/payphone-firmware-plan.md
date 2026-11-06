# Payphone features: firmware plan

**Plan only: none of this is built.** It describes a future Settings → **Payphone**
tab, marked *Experimental*, for Analog Redial boards in a payphone. Items marked
**[verify]** are from memory and need checking against a real phone.

## The tab

- **Enable payphone features** (checkbox, off by default). Off, the board behaves
  exactly as it does today and nothing below runs.
- An **Experimental** badge on the tab, like the other in-progress features.

## Where coins are detected

| Source | How | Needs |
|---|---|---|
| **Coin board inputs** | U1 on the coin board (I2C 0x39): nickel, dime, quarter and coin 4, each a short low pulse | the coin board |
| **Coin tones on the line** | Phones with a totalizer send tones down the line when a coin drops: 1700 + 2200 Hz together, one 66 ms beep for a nickel, two for a dime, five 33 ms beeps for a quarter **[verify]**. The main board already hears the line, so this is a tone detector next to the DTMF one | nothing extra |
| Both | either one counts; a coin seen by both within about 0.5 s counts once | |

Each source has its own checkbox. Tone detection is useful even without a coin
board, for phones whose totalizer still works.

Whether most converted payphones still send coin tones is unknown: a single-slot
phone's totalizer needs line current to work, and some home conversions bypass the coin
mechanism entirely **[verify on real phones]**. It's firmware only, so it costs nothing
to offer.

## What happens on a coin

In the order they'd appear on the tab:

1. **Coin required for dial tone.** Off-hook with no credit plays a message instead
   of dial tone ("Please deposit 10 cents", text editable, spoken by the same
   prompt system as the operator message), repeated every few seconds. Dial tone
   starts once the credit reaches the set price. Options: the price, and numbers
   that are always free (the operator code, the board's own dial codes, emergency
   numbers).
2. **Message after any coin**: a chosen clip or spoken text when any coin is seen.
3. **Message after a specific coin**: a clip per coin type (nickel, dime, quarter,
   coin 4), overriding the one above.
4. **Detect coin tones** (the source above), with a sensitivity setting.

## Other features worth having

- **Credit and timing:** price per call, and a time limit with a warning ("please
  deposit 5 cents for the next 3 minutes"), like an old payphone.
- **Coin collect and return** (coin board's relay): collect when a call is answered,
  return when the caller hangs up before an answer or the call fails. A per-call
  override, and **test buttons** for collect and return.
- **Coin box counter:** total coins and value since the last reset, with a reset
  button ("emptied the box").
- **Simulate a coin** button for testing without a phone.
- **Coin value for coin 4** (50c, $1 or "coin present").
- **Home Assistant event** on each coin, if the integration is on.

## Logging

A log on the tab, newest first, kept in RAM (and on microSD if there is one):

| Time | Line | Event | Detail |
|---|---|---|---|
| 14:02:11 | 0 | coin | quarter, coin board |
| 14:02:13 | 0 | coin | dime, line tones |
| 14:05:40 | 0 | collect | 119 V, 82 mA, 45 ms (coin board) |

Coin events from each source, credit changes, and (with the coin board) every
collect and return with its voltage, current and pulse length from U4, plus faults
(no current, current that didn't stop, HV not reached).

## Notes for the build

- Coin board driver: poll U1 every 2-5 ms over I2C and debounce (coin-board-schematic.md
  section 8); write U1's output register before its direction register (section 4).
- Firing sequence and limits: coin-board-schematic.md section 7, plus the 19 V mode
  (S2_OFF) for low-voltage coils in section 6.
- Settings in NVS, like the other tabs; the tab is hidden when the feature is off,
  except the enable checkbox.
