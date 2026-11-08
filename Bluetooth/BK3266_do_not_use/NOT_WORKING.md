# BK3266 / BH519: not working as intended (2026-10-20)

The module sold as a BK3266 stereo Bluetooth board (Bluetooth name **BH519**) powers on (wake pin held high about 3 s), blinks,
pairs with a phone and carries audio, but **its UART stays silent**: no boot messages and no replies to the binary
protocol or to the text commands (`BT+`, `AT`, `AT+`, `COM+`) at any baud rate from 1200 to 460800, with TX and RX both ways,
on two different boards. No call control means it cannot be used as a hands-free module here.
The research below and the driver were written for the BK3266SR "transmit/receive integrated" variant with a binary UART protocol;
this module is not that, or has its UART disabled. Full bench log: `archive/hardware/bk3266.md`.
Use the BM64 (tested) or the DFRobot DFR0781 (to be tested) instead: see `../README.md`.
