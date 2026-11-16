# Payphone types and coin control methods

**Status: planning and initial implementation.** Green box tone generation is
in the firmware; nothing here has been tested with a real payphone yet.

This doc covers which payphones work with Analog Redial, what hardware they
need, and what the firmware's "Collect and return method" setting does.

---

## 1. Quick reference

| Phone type | Era | Coin detection | Collect / return | Line seizure | What you need |
|---|---|---|---|---|---|
| **Three-slot** (WE 233G, 234G) | ~1950s--1970s | Totalizer sends coin tones (1700+2200 Hz) | **Green box tones** (2200+2600 Hz) from the CO | Ground start | Tip/ring from Analog Redial. No extra hardware if the totalizer works. |
| **Single-slot, tone** (WE 1C early) | ~1970s--1980s | Totalizer sends coin tones | **Green box tones** | Ground start | Same as three-slot. |
| **Single-slot, ACTS** (WE 1D, 1D2) | ~1980s--2000s | Totalizer sends coin tones **[verify]** | **DC voltage** (+/-130 V polarity reversal) via ACTS | Ground start | Coin board for collect/return. Coin tones may still work for detection if the totalizer is intact. |
| **COCOT** (Protel, Elcotel, Palco...) | ~1980s--2000s | Microprocessor, proprietary | Proprietary protocol | Varies | Usually not compatible without bypassing the phone's controller. |
| **Decorator / converted** | Any | None (mechanism removed) | None needed | Loop start | Just tip/ring -- works like a regular phone. |

**[verify]** = not confirmed on real hardware; check on the bench.

---

## 2. Phone types in detail

### 2a. Three-slot phones (the "classic" payphone)

Western Electric 233G, 234G, and similar. Three separate coin slots at the
top (nickel, dime, quarter). A mechanical **totalizer** counts the coins and
sends tones down the line:

- **Coin tones:** 1700 + 2200 Hz together. One 66 ms beep for a nickel, two
  for a dime, five shorter ones for a quarter. The firmware's existing coin
  tone detector (`coin_tone.c`) already hears these.

- **Collect / return:** The CO sends **green box tones** (2200 + 2600 Hz)
  back down the line. The totalizer has a tone decoder that fires the coin
  relay: one burst = collect (coins drop into the vault), two bursts = return
  (coins come back to the customer). Three bursts = operator ringback.

- **Line seizure:** Ground start (the phone grounds RING when the handset is
  lifted). See `ground-start.md`.

**What you need:** Tip and ring from the AG1171, and the firmware's "Green box
tones" collect/return method. No coin board, no extra hardware -- as long as
the phone's totalizer and its tone decoder are intact.

**Common issues:** Salvaged three-slot phones are old. The totalizer mechanism
may be worn, corroded, or missing parts. If the tone decoder doesn't respond,
the coin board can fire the relay electrically as a fallback.

### 2b. Single-slot phones, tone era

Western Electric 1C (early versions) and similar single-slot predecessors to
the ACTS phones. One coin slot accepts all denominations. Still has a
totalizer with coin tones and a green-box tone decoder.

Same as three-slot: green box tones work if the totalizer is intact.

### 2c. Single-slot phones, ACTS era (the user's phone)

Western Electric 1D, 1D2, and the AT&T/Lucent successors. The phone most
people think of as "a payphone." Single coin slot, HD550002C coin relay
mechanism.

These were designed for **ACTS** (Automated Coin Toll System), which
controlled the coin relay by **DC voltage polarity reversal** -- approximately
+130 V to collect, -130 V to return -- not by tones. The coin board
(`PayPhone/Payphone_coin_board_v1/`) generates this voltage.

**Do they also respond to green box tones?** The 1D-series phones still have a
totalizer that sends coin tones (1700+2200 Hz) when coins drop, and the ACTS
system listened for those tones to count the deposit. Whether the totalizer
also has a tone decoder for the 2200+2600 Hz collect/return signal depends on
the specific phone and vintage -- some do (for backward compatibility with
operator-controlled toll positions), some don't. **[verify on the bench]**

**What you need:**

- **Coin detection:** The firmware's coin tone detector works if the totalizer
  still sends tones. The coin board's physical coin switches are the
  alternative (more reliable, works even if the totalizer is dead).

- **Collect / return:** The coin board, with its HV boost converter and
  polarity relay. Set the firmware to "Coin board" or "Both" (tries green box
  tones too, in case the phone responds).

- **Line seizure:** Ground start. See `ground-start.md`.

### 2d. COCOT (Customer-Owned Coin-Operated Telephone)

Protel, Elcotel, Palco, Quadax, and other privately owned payphones from the
deregulation era (~1984 onward). These have a microprocessor inside the phone
that handles coin counting, call timing, and rate tables. They talk to a
central server over the phone line using proprietary modem protocols.

**Not compatible with green box tones or the coin board** without significant
modification. The phone's controller won't fire the relay in response to
external signals -- it makes its own decisions.

To use a COCOT with Analog Redial:
- Bypass or remove the phone's controller board
- Wire the coin switches and relay directly (the coin board can drive them)
- Or just use it as a decorator phone (no coin control)

### 2e. Decorator / converted phones

A regular telephone handset in a payphone housing, or a payphone with the coin
mechanism removed. No totalizer, no coin relay, no coin slots (or the slots
are blocked).

Works as a regular phone on Analog Redial -- just tip and ring, loop start.
No payphone features needed in the firmware.

---

## 3. Green box tones: specification

The "green box" is the CO-to-phone coin control signal used by the Bell System
from the 1950s through the ACTS transition. The name comes from the phreaking
community (it was the color of the device that could generate these tones).

### Frequencies

**2200 Hz + 2600 Hz**, equal amplitude, sent simultaneously.

These are the same frequencies used for MF (multi-frequency) in-band trunk
signaling (SF, single-frequency 2600 Hz, and the 2200+2600 Hz "coin control"
pair). On a real toll network, these would be filtered out before reaching the
subscriber. On Analog Redial's direct tip/ring connection, they go straight to
the phone.

### Burst patterns

| Action | Bursts | Timing |
|---|---|---|
| **Collect** | 1 | One burst, ~700 ms |
| **Return** | 2 | Two bursts, ~700 ms each, ~200 ms gap |
| **Ringback** | 3 | Three bursts, ~700 ms each, ~200 ms gaps |

**[verify]** The exact timing varies by source (500--1000 ms bursts, 150--300 ms
gaps are all cited). The firmware defaults to 700 ms on / 200 ms off, which
matches the most common references. If a phone's totalizer doesn't respond,
try shorter bursts (some decoders are tuned to ~500 ms) or check the
amplitude.

### Amplitude

The firmware sends the tones at 0.8 of full scale (same ballpark as DTMF).
This should be well above the totalizer's detection threshold. If the phone
doesn't respond, the tone level can be checked at the phone's terminals with
an oscilloscope or audio analyzer.

---

## 4. Firmware settings

### Collect and return method (Settings -> Payphone)

| Setting | What it does | Hardware needed |
|---|---|---|
| **None** | Coins are counted and logged but not collected or returned. | Nothing beyond tip/ring. |
| **Green box tones** | Sends 2200+2600 Hz tone bursts down the phone line. | Tip/ring only. Phone must have a working totalizer with tone decoder. |
| **Coin board** | The coin board fires the relay electrically over I2C. | Coin board connected to J11. |
| **Both** | Sends green box tones AND fires the coin board relay. | Coin board + tip/ring. |

**Default: None.** Change it after confirming which method your phone responds to.

### When tones are sent

- **Collect:** When a call placed from the payphone is answered (the coins
  drop into the vault). One burst.
- **Return:** When the handset goes down without a completed call (the coins
  come back). Two bursts.

The tones play on the phone's line. During a live call (collect on answer),
the caller hears a brief interruption (~0.7 s of silence while the tones
play). In the real Bell System, the ACTS equipment had a separate signaling
path; Analog Redial shares the audio channel, so the interruption is
unavoidable. For most calls it's barely noticeable.

### Coin detection

Independent of the collect/return method:

- **Coin tones** (1700+2200 Hz): The firmware listens for them on the line.
  Works if the phone's totalizer sends tones when a coin drops.
- **Coin switches** (via coin board): Physical microswitches in the phone,
  read by the coin board's I/O expander. Works regardless of the totalizer.
- **Simulated** (web UI): The "simulate a coin" buttons for testing.

Both coin tones and coin switches can be active at the same time (on different
lines, or as redundant sources).

---

## 5. Choosing the right setup

### "I have a Western Electric three-slot phone (233G, 234G)"

1. Connect tip and ring from the AG1171 to the phone's line terminals.
2. Settings -> Payphone -> enable, set collect/return to **Green box tones**.
3. Enable **coin tone detection**.
4. Test: drop a coin, check the log for "5 c coin (coin tones)".
5. Test: simulate a collect from the web UI, listen for the relay click.
6. If the relay doesn't fire on green box tones, the totalizer's tone decoder
   may be dead. Use the coin board as a fallback (**Both** or **Coin board**).
7. Ground start: see `ground-start.md`. If the phone doesn't get dial tone on
   pickup, it may need the ground-start hardware.

### "I have a Western Electric single-slot phone (1D, 1D2)"

1. Connect tip and ring. Try green box tones first (free, no extra hardware).
2. If the phone responds to green box tones: done, use **Green box tones**.
3. If not: install the coin board, set to **Coin board** or **Both**.
4. For coin detection: enable coin tones. If the totalizer doesn't send them,
   the coin board's switches are the fallback.
5. Ground start: almost certainly needed. See `ground-start.md`.

### "I have a Protel / Elcotel / other COCOT"

1. These phones have their own controller. Green box tones won't work.
2. Options: bypass the controller and wire the coin switches/relay to the coin
   board, or remove the coin mechanism and use it as a decorator phone.
3. If you bypass the controller: set to **Coin board**, use coin switches for
   detection.

### "I just want a cool-looking phone, no coins"

1. Connect tip and ring. Leave payphone features off.
2. It works like any other phone on Analog Redial.

---

## 6. Open questions

- [ ] Do WE 1D/1D2 phones respond to green box tones? Test on the bench with
  a known-good 1D and the firmware's tone output.
- [ ] Exact burst timing for reliable detection across phone models. The
  firmware defaults (700 ms on / 200 ms off) may need tuning.
- [ ] Should the firmware support adjustable burst timing in the web UI, or
  is a single set of defaults enough?
- [ ] Ringback (3 bursts) is implemented in the tone generator but not yet
  exposed in the UI or telephony flow. Add it when there's a use case.
- [ ] Ground-start integration: the collect/return method and ground-start
  line seizure are independent features. Both are needed for a complete
  payphone setup. See `ground-start.md`.
