# Payphone types

**In progress.** Nothing on this page has run on the coin board yet: the board isn't
built, and its firmware has only run against simulated chips.

Payphones differ in how they report coins and how their coin relay is driven. This page
collects what each known phone needs, one row per phone, so the firmware can later
offer a phone type to pick instead of setting each value by hand. Until then, the
values in the last table are the ones to set on the Payphone tab and on the coin
board's jumpers.

To add a phone, add a row to each table, mark anything not measured **[verify]**,
and note in the last column where the numbers came from.

## 1. Identification

| Phone | Housing | Controller board | Made | Source |
|---|---|---|---|---|
| **GTE / Quadrum 120B style** (the project's first phone) | GTE / Quadrum, coin return lower right | **HB-1301-B** (40-pin DIP, two CP Clare reed relays, 310 V gas tube); smart or dumb not known **[verify]** | week 51, 1997 | photos and bench tests, 2026-10-24 to 2026-10-31 |

## 2. Coin detection

| Phone | Sensor | Cable | Wires | Signal | Coins | Coin tones |
|---|---|---|---|---|---|---|
| GTE / Quadrum 120B style | passive coin switches (measured) | 4-wire, Molex KK 396 (coin board J3) | green common; white quarter, red dime, blue nickel | contact to common, closes briefly per coin | 5, 10, 25 cents | not checked **[verify]** |

## 3. Coin relay

| Phone | Relay | Coils | Wires | Resistance | Operates at | Direction | Switch-off | Cable |
|---|---|---|---|---|---|---|---|---|
| GTE / Quadrum 120B style | **HD550002C** (QTI) | one, polarized | white and green to the coil path; red not connected | about 1 k at rest; 0.1 ohm across the coil once operated | 55 V no; **60 V yes** | white positive: lever left, **collect** (bench; confirm in the phone) **[verify]**; reversed: lever right, return | a contact on the white-green path opens as it operates, so it switches itself off | 3-wire, Molex KK 396 (coin board J2) |

## 4. Coin board settings

| Phone | Relay connector | J10 (two-coil) | HV (J14, J7-J9) | Pulse | Swap | Coin 4 | Tested |
|---|---|---|---|---|---|---|---|
| GTE / Quadrum 120B style | J2 | off | fit J14; start at 60 V (J14 + J9), up to 78 V (J14 + J8) if it doesn't move reliably | 100 ms (default); the relay cuts its own current | off, until a Collect test moves the lever the wrong way | 0 (not fitted) | **no** |

Notes on the columns:

- **HV** is set by shunts, not by firmware: [coin-board-schematic.md](coin-board-schematic.md)
  section 6 has the table. Without J14 it stays at about 19 V, for 12-24 V coils.
- **Pulse** and **Swap** are on the Payphone tab's coin board card. Swap reverses which
  way Collect and Return drive the relay.
- **Coin 4** is the cents value of the fourth coin input (0 = only logged).

## Still to find out for the first phone

- Whether the HB-1301-B board is smart (counts coins itself) or dumb (central-office
  controlled), and whether the phone sends coin tones on the line.
- Which relay direction the phone's own levers treat as collect, with the relay back in
  the phone.
- What resets the relay's latched levers.
