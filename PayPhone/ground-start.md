# Ground-start signaling for the payphone coin board

**Status: planning only.** Nothing here is built or tested. This doc
captures the design intent so it can be picked up later.

---

## 1. What is ground start and why does it matter?

### Loop start (what the AG1171 does now)

Phone goes off-hook, closing the tip/ring loop. The AG1171 detects current
flow (SHK pin goes high) and the firmware provides dial tone. For incoming
calls, the AG1171 applies ringing; the phone answers by going off-hook.

The problem: **glare**. Both sides can seize the line at the same instant
(CO starts ringing while the phone goes off-hook for an outgoing call).
On a home phone this is rare and harmless. On a PBX trunk or payphone it
causes misrouted calls and confused coin mechanisms.

### Ground start

Each side signals on a **different wire**, so seizure is unambiguous:

| Direction | Who | Action | Other side detects |
|---|---|---|---|
| **Outgoing** (phone -> CO) | Phone grounds **RING** | Current flows: battery -> RING -> earth ground | CO sees RING current without TIP current = "station seizure" |
| **Incoming** (CO -> phone) | CO grounds **TIP** | TIP goes to ~0 V | Phone sees TIP ground = "incoming call", then CO rings |

After seizure is acknowledged, both sides close the loop and the call
proceeds as normal loop start.

### Why it matters for the payphone

Western Electric single-slot coin phones (1D, 2D, and the HD550002C
mechanism) use ground start. When the customer lifts the handset:

1. The phone grounds RING through the coin relay mechanism
2. The CO detects RING ground and connects the line
3. CO provides dial tone
4. The coin mechanism controls collect/refund based on CO signaling

If the payphone runs on a loop-start line, the RING-ground seizure may or
may not be detected correctly by the AG1171, because grounding one wire is
not the same as closing a balanced loop through both wires. Some setups
work by accident; others don't.

---

## 2. Hardware additions

Two things are needed on the coin board, plus an earth-ground connection.

### 2a. TIP ground switch (CO -> phone, incoming calls)

The CO must be able to ground the TIP lead to signal an incoming call
before applying ringing.

```
TIP (from AG1171) ----+---- to phone
                      |
                  K_tip (relay or MOSFET)
                      |
                     GND
```

**Options:**

| | Relay | MOSFET |
|---|---|---|
| Part | SPST, 5 V coil, >= 60 V contacts (e.g. G6K-2F, one pole) | N-channel, >= 60 V Vds, SOT-23 (e.g. BSS138 or AO3400A) |
| Drive | MCP23008 GPIO -> NPN driver (same circuit as K1's Q2) + flyback diode | MCP23008 GPIO -> gate resistor; add a current-limit resistor in series with drain-to-GND path |
| Current | 20-30 mA through contacts (phone's TIP ground draw) | Same, plus Rds(on) drop |
| Galvanic isolation | Yes | No |
| Board area | Larger | Smaller (SOT-23 + 2 resistors) |

A relay is simpler and avoids any worry about MOSFET body-diode paths or
voltage spikes. A second G6S-2 pole (K1 already has a spare pole if it's
a DPDT) might work if the contact rating is sufficient.

**Check:** can the spare pole of K1 (G6S-2) be repurposed for TIP ground?
K1's coil is already driven by Q2. If so, no new relay or driver needed —
just route K1's second pole between TIP and GND and control the timing in
firmware. The coin board's K1 is DPDT; currently only one pole is used for
polarity switching.

### 2b. RING ground detector (phone -> CO, outgoing calls)

Detect when the payphone grounds RING to seize the line.

```
RING (from AG1171) ---+---- to phone
                      |
                   R_top (e.g. 470k)
                      |
                      +--- to ADS1115 AIN channel (or comparator)
                      |
                   R_bot (e.g. 10k)
                      |
                     GND
```

- In idle state, RING sits at about -24 V to -48 V (AG1171 battery).
  The divider scales this to a small negative voltage the ADS1115 can
  read (it handles +/-2.048 V in differential mode, or 0-VDD in
  single-ended).
- When the phone grounds RING, the voltage jumps to ~0 V. The divider
  output goes to ~0 V too.
- The ADS1115 is already on the coin board with spare channels (AIN2 or
  AIN3). No new IC needed.
- **Distinguish from normal off-hook:** RING ground = RING near 0 V but
  SHK still low (no loop current). Normal off-hook = SHK high (loop
  current through both wires). Firmware checks both.

**Alternative:** use an LM393 comparator with a threshold set at about
half the idle RING voltage, output to a MCP23008 GPIO interrupt pin.
Faster response than ADS1115 polling, but adds a part.

### 2c. Earth ground connection

Ground start requires an **earth ground** reference. The phone's chassis
(metal housing on WE payphones) connects to building ground. The CO has
its own ground.

In the Analog Redial setup, the phone and board share the same ground
(through the 5 V power supply), so "grounding RING" means connecting RING
to the board's GND. This **simplifies** the detection — no ground
potential difference to worry about.

**Requirement:** the payphone's earth-ground terminal must connect to the
coin board's GND. Add a screw terminal or spade lug pad near J2/J3 for
this wire. On a WE payphone, the ground lug is inside the housing.

---

## 3. Bill of materials impact

| Part | Purpose | Est. cost |
|---|---|---|
| 1 relay (SPST, 5 V, >= 60 V) or 1 MOSFET (60 V+ SOT-23) | TIP ground switch | ~$1-2 |
| 1 NPN driver + flyback diode (if relay) | Relay control | ~$0.20 |
| 2 resistors (voltage divider) | RING sense | ~$0.02 |
| 1 ADS1115 channel (already on board) | RING voltage reading | $0 |
| 1 screw terminal or spade pad | Earth ground | ~$0.10 |
| **Total** | | **~$1-2** |

One more MCP23008 GPIO used (for TIP ground control). The MCP23008 has
8 GPIOs; check current allocation for a spare.

---

## 4. Firmware changes

### New config option

Settings -> Hardware -> Line mode: **Loop start** (default) | **Ground start**

### New telephony FSM states

| State | Entry condition | Action | Exit |
|---|---|---|---|
| **RING_GROUND_DETECTED** | RING voltage near 0 V AND SHK low (no loop) | Phone seized line via ground start; provide dial tone | Phone closes loop (SHK high) -> CONNECTED |
| **TIP_GROUND_APPLIED** | Incoming call, ground-start mode | Close TIP ground switch, then apply ringing via AG1171 RM pin | Phone answers (SHK high) -> release TIP ground -> CONNECTED |

### Detection logic (outgoing call, phone -> CO)

```
Every ~50 ms (ADS1115 poll or comparator interrupt):
    if line_mode == GROUND_START:
        ring_voltage = read_ring_adc()
        shk = read_shk_pin()

        if ring_voltage > RING_GROUND_THRESHOLD and shk == LOW:
            # RING grounded, no loop current = station seizure
            start_dial_tone()
            state = RING_GROUND_DETECTED

        # Once in RING_GROUND_DETECTED, wait for loop closure:
        if state == RING_GROUND_DETECTED and shk == HIGH:
            # Phone closed the loop, normal call now
            state = CONNECTED
```

### Incoming call sequence (CO -> phone)

```
1. Close TIP ground switch (K_tip or MOSFET)
2. Wait ~200 ms for phone to detect TIP ground
3. Apply ringing (AG1171 RM pin)
4. On answer (SHK goes high):
   a. Stop ringing
   b. Open TIP ground switch
   c. Connect audio path
```

### Interaction with coin mechanism

The ground-start seizure event is when the firmware should start listening
for coin switch closures (Nickel, Dime, Quarter inputs). The sequence:

1. Customer lifts handset -> phone grounds RING (ground-start seizure)
2. Firmware detects seizure, plays "deposit coin" prompt or dial tone
3. Customer deposits coin -> coin switch closure detected on MCP23008 GPIO
4. Firmware plays dial tone (or second dial tone, depending on mode)
5. Customer dials -> DTMF or pulse dialing proceeds normally

---

## 5. PCB layout notes

- **TIP ground switch** near the TIP trace from J2/J3 to the AG1171.
  If using K1's spare pole, just route K1's second pole contacts.
- **RING sense divider** near the RING trace, short run to ADS1115 AIN
  input. Keep the high-impedance divider output away from switching noise
  (L2, Qa, D2).
- **Earth ground pad** near J2/J3, on the board edge for easy wire access.
- All new parts are low-voltage, low-current. No HV clearance needed
  beyond what's already required for the AG1171 tip/ring traces.

---

## 6. Open questions

- [ ] Does K1's spare DPDT pole have enough contact rating for TIP ground
  current (~30 mA at ~48 V)? The G6S-2 is rated 2 A at 30 VDC — yes,
  easily. **This is probably the simplest path: no new relay.**
- [ ] Which ADS1115 channel is free for RING sensing? Check current
  allocation (AIN0 = HV monitor, AIN1 = 20 V monitor, AIN2 = ISENSE,
  AIN3 = spare?).
- [ ] Does the AG1171 behave correctly when TIP is externally grounded
  (incoming call signaling)? It may see a fault condition. Test on bench.
- [ ] Can the existing SHK detection reliably distinguish "RING grounded"
  from "loop closed"? If not, the ADC reading is the discriminator.
- [ ] Polling rate: is ~50 ms (ADS1115 at 128 SPS cycling 4 channels) fast
  enough to catch a RING ground event? Phone holds RING ground for
  several hundred ms, so probably yes.
- [ ] Payphone-specific: does the WE 1D/2D coin mechanism expect a
  specific timing on TIP ground before ringing? Check the BSP
  (Bell System Practice) documents.

---

## 7. Scope

**Add to the payphone coin board only.** The main Analog Redial board
(home phones) does not need ground start — residential phones are all
loop start. Implement as a per-line firmware config so it can be toggled
without a hardware change (the hardware just needs to be present on the
board).
