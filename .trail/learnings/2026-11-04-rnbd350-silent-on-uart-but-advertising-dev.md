---
title: RNBD350 silent on UART but advertising = open solder joint; test each direction over BLE
author: Dev
date: 2026-11-04
branch: rnbd350
cost: { minutes: 1, tokens: 0 }
id: l-57e3ffda
scope: repo
status: active
src: agent
touches:
  - src/bluetooth.cpp
  - src/logic/ble_status.cpp
  - tools/basetest/README.md
---

## Decided
2026-11-04 bench, after the RN4870 -> RNBD350 swap on base and remote: both modules advertised (factory names RNBD350_xxxx)
but Bluetooth::init() in src/bluetooth.cpp saw no %REBOOT% and no CMD> reply to $$$. The USART2 config was correct on both boards.
Testing each wire separately located the faults, which were open joints, not firmware:
- module TX -> MCU: write to the Transparent UART char 49535343-8841-... from the PC (bleak). The bytes must appear in
  Bluetooth::_rxRing (src/bluetooth.cpp; remote: Bluetooth::(anon)::rxBuf). Read it with openocd mdb while the core runs.
- MCU -> module RX: connect from the PC, subscribe to 49535343-1e4d-..., write bytes into USART2->TDR via openocd. They must arrive as notifications.
- RST (PA1) -> NMCLR: pulse RST through the probe while the PC is connected. The link must drop.
Real RNBD350 event text, now parsed by src/logic/ble_status.cpp: %CONNECT,0,10B1DF04F934,0072% %STREAM_OPEN% %ERR_PHYUPDATE,0072%
%CONN_PARAM,0010,0000,0048,0072% %PHY_UPDATED,0072,02,02% %DISCONNECT,0072%.
## Tried and ruled out
- USART TX/RX swap test on the base (CR2.SWAP): it floats the module RX low, the module stops advertising, and the result is confounded.
## Watch out
- Pin level tests with an MCU pull-down are inconclusive: the module's inputs have 10-30k internal pull-ups and NMCLR has its own 10k.
- Never run st-info --probe on this bench (tools/basetest/README.md): it halts the cores, and it left the base ST-Link enumerating as VID 0000 until replugged.
