[Home](../README.md) · [1 · Get a board](board.md) · [2 · Parts & assembly](bom.md) · [3 · Flash & test](flashing.md) · [4 · Set up & use](setup.md) · **Troubleshooting**

# Troubleshooting

Find the symptom below and work down its list; the likeliest causes come first.
Three tools help with almost everything:

- **Settings → Diagnostics** in the web interface has the log, the hardware
  self-test and bench tests that ring a bell or play a tone.
- **The serial console** works without Wi-Fi ([commands](flashing.md#6-test-the-hardware)).
  Start with `selftest`, which tests the hook and audio paths separately.
- **A second phone.** Trying a phone you know works tells you straight away
  whether the fault is in the board or the phone.

> ⚠️ **Phone lines carry real voltage:** about 48 V DC off-hook and about 60 V AC
> while ringing, with peaks near 90 V. Unplug the phone from the board before you
> work inside it, and don't touch its terminals while it rings.

## A phone won't ring

Also use this list if the bell only rings weakly.

1. **Check the handset is on the hook.** The board never rings a lifted handset.
   A hold button on the phone can also keep the line off-hook after you hang up.
   The line should show as idle on the Status page.
2. **Check quiet hours.** They keep the bell silent overnight (Settings → General).
3. **Ring it directly.** Use **Ring a phone** under Settings → Diagnostics, so
   that the test doesn't depend on a call or on Wi-Fi.
4. **Check the phone's ringer control.** Many phones have a volume wheel or an
   off switch on the base or side.
5. **Try a phone you know rings.** If that one rings well, the board is fine and
   the problem is in the first phone. Carry on below.
6. **Run the ring frequency test.** See the next section. Old phones that were
   used on party lines may only ring properly at one frequency.
7. **Check the ringer's wiring.** Unplug the phone and trace the ringer's coil and
   capacitor back to the line cord. They should connect across the two line wires.
   Party-line phones often connected one side to a separate ground wire instead,
   and that wire isn't connected on a two-wire line.
8. **Check the mechanics.** Flick the clapper by hand: it should swing freely and
   hit both gongs. Gongs are often mounted off-centre, so loosening one and turning
   it changes the gap.

### Ring frequency test

A normal ring is 20 Hz, but a party line could ring one phone without ringing its
neighbours by giving each phone a ringer tuned to a different frequency. On 20 Hz,
one of those ringers barely moves, or doesn't ring at all, even though nothing is
broken. A heavy weight on the clapper or a single ringer coil can be a clue, but
the test is the way to be sure.

1. Hang up the phone. Go to **Settings → Diagnostics → Ring frequency test**, choose
   the line, and press **Step through all**. It rings each frequency from 16 to
   66 Hz for 3 seconds with a 2-second pause, about a minute in all.
2. Watch the page, which shows the frequency ringing now, and note where the bell
   is clearly loudest.
3. Choose that frequency and press **Ring this one** to confirm it, then try 20 Hz
   to compare.
4. If it isn't 20 Hz, save it under **Settings → Hardware → Ring frequency**. It
   takes effect at once and is kept after a restart.

| What you hear | What it means |
|---|---|
| One frequency is clearly loudest | The ringer is tuned to it. Save that frequency |
| Loudest at 20 Hz, but still weak | Tuning isn't the problem. Check the wiring and mechanics |
| Weak or silent at every frequency | Check the wiring and mechanics, then try another phone |

The setting belongs to the line, not the phone. A normal phone plugged into that
line later will ring weakly until you set it back to 20 Hz.

## No dial tone

1. **Check the line is set up.** A two-line board set to one line never uses its
   second line. See Settings → General → Board hardware.
2. **Check whether the board notices the handset.** Watch the line's state on the
   Status page, or run `hookmon 20` on the console, as you lift and replace it.
3. **If it never notices the handset** and this is a v4 board, check that R14 and
   R12 were [removed](building.md#2-v4-only-remove-two-resistors-first). On any board,
   check the phone cable and the phone's hook switch.
4. **If it notices but there's no sound,** run `selftest`. It checks the audio path
   separately. `codec_detect` confirms that the audio chip answers.
5. **If the phone's dial tone is silent but a second phone's works,** the fault is
   in the first phone or its cord.

## Dialing goes wrong

- **Digits come out wrong on a rotary phone:** run `pulsemon` on the console and dial.
  It shows each pulse and its timing, which shows up a dirty or slow dial.
- **Touch-tone digits aren't recognized:** run `tonemon` and press some keys to see
  which digits the board hears. If it misses them, try another phone.
- **A code does something unexpected:** look it up in Settings → Dial Codes. The
  Status page also lists what happened recently, including misdials.

## Calls between boards

- **Dialing a number rings your own phone or plays something else:** this board's
  dial codes win over other boards' numbers. On a two-line board, 321 calls the
  other line. To use 321 as a number, change that code in Settings → Dial Codes.
- **The call fails and you go straight back to dial tone:** the other phone's line
  is probably in use. Check that it's hung up and that no hold button is down.
- **A number won't save because it clashes:** change the dial code it clashes with
  first, in Settings → Dial Codes.
- **The other board isn't listed or can't be reached:** both boards need to be on
  the same network. Guest networks often keep devices apart. See
  [VoIP setup](voip-setup.md) for calls through a server.

## Sound problems

- **Harsh dial tone:** the earpiece level is too high and the line's audio input is
  distorting. Run the line audio level check under Settings → Diagnostics, and use
  its recommended level.
- **Static or dropouts on calls between boards:** look at the Wi-Fi signal on the
  Status page and move the board closer to the router if it's weak. Calling the
  other line on a two-line board uses no Wi-Fi, so it helps tell the two apart.
- **You hear your own voice as an echo:** measure and turn on echo cancellation under
  Settings → Phone. Pick up the handset first and keep the room quiet while it measures.
- **Audio cuts out and the board restarts:** see [power problems](#power-problems).

## Can't reach the board

1. **Check the status light** ([what each colour means](setup.md#7-the-status-light)).
   A green slow blink that continues after boot means setup mode: join the board's own
   Wi-Fi network as in [Get it on your Wi-Fi](setup.md#1-get-it-on-your-wi-fi).
2. **Try its IP address** instead of `name.local`. Your router's device list shows
   it, or run `wifi_status` on the console. Not every device or browser resolves
   `.local` names.
3. **Check the network.** The board uses 2.4 GHz Wi-Fi only, and your computer has
   to be on a network that can reach it.
4. **Use rescue mode.** Hold BOOT for 3 seconds with the board running. It brings up
   its own network, keeping your saved ones. Browse to 192.168.4.1. See
   [The BOOT button](setup.md#8-the-boot-button-rescue-and-reset), which also covers
   a forgotten web password.

## Power problems

- **Amber double blink, or a brownout banner:** the board restarted because its
  supply dipped. Use a 5 V supply rated 2 A or more. Some battery packs and weak USB
  ports can't keep up, especially while ringing.
- **A v5 board with a 30-pin clone module won't power up properly:** on a v5 board
  (not v5.1), the clone's bottom-right 5V pin must be removed, because the footprint
  ties it to GND. See [the clone module notes](bom.md#microcontroller-module-32-pin-or-30-pin-clone).

## Updating and flashing

- **"Check for updates" fails:** the browser does the download, so the computer or
  phone you're using needs internet access, not just a connection to the board.
- **An update seems to have been undone:** if a new version doesn't start, the board
  goes back to the old one by itself. Check the log under Settings → Diagnostics.
- **The computer can't see the board:** use the DevKit's USB-C port marked **USB**, not
  **UART**, and a cable that carries data. Close any serial monitor that is still
  holding the port.
- **Flashing fails or the board stays in flashing mode afterwards:** try again once or
  twice. If it's still stuck, unplug the USB cable and plug it back in.
- **The serial monitor restarts the board:** opening the port resets it. That's normal
  for this chip's USB, and it ends any call in progress.

---

**Back to: [Home](../README.md) · [Set up & use](setup.md)**
