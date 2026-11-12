---
title: "Indicator RX must come back at USART TC, not DMA TC; LA leads D0-D3 can be reversed"
author: Dev
date: 2026-11-12
branch: main
cost: { minutes: 1, tokens: 16503 }
id: l-b0fe237b
scope: repo
status: active
src: agent
touches:
  - src/display.cpp
  - tools/basetest/la.py
  - TODO.md
---

## Decided
- src/display.cpp: the RX DMA is switched off when a frame starts and back on only in USART1_IRQHandler on TC (TCIE set in the TX DMA TC ISR), after RQR RXFRQ and clearing ORE/FE/NE (restartRx). DMA TC fires when the last byte is handed to TDR, about 2 bytes (~17 ms at 1200 Bd) before it leaves the wire; re-enabling there let our own echo (PA10 hears PA9) fall into the RX buffer and misalign the next foreign frame.
- Flag check and quiet-bus wait live in src/logic/indicator_bus.* (host tests test_indicator_bus.cpp). display::rxActivityMs/rxActivityCount are written by the ISRs; tests poke them through the probe to simulate a busy bus because the bench has no display.

## Tried and ruled out
- After flashing, every LA display test decoded zero frames. It was not the firmware: HEAD display.cpp failed the same way. The D0-D3 leads were reversed on the bench (1200 Bd frames on D2, 115200 Bd BLE bursts on D0/D1). Check raw edges per channel before blaming code.

## Watch out
- tools/basetest/la.py honours BASETEST_LA_CH="disp_tx=D2,disp_rx=D3,base_ble_tx=D0,base_ble_rx=D1" for a rewired bench.
- The echo rejection is verified only as the RX-off window (73-105 ms); a real display bus is still unchecked (TODO.md).
