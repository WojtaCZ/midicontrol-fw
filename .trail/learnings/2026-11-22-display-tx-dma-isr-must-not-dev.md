---
title: "Display TX DMA ISR must not clear USART TCCF: a late ISR loses TC and txBusy sticks forever"
author: Dev
date: 2026-11-22
branch: backlog/phase-c
cost: { minutes: 7, tokens: 75320 }
id: l-3a6b6b8d
scope: repo
status: active
src: agent
touches:
  - CMakeLists.txt
  - TODO.md
  - inc/led.hpp
  - src/led.cpp
  - src/logic/led_logic.hpp
  - test/host/test_led_logic.cpp
  - docs/SYSTEM.md
  - src/logic/recovery.cpp
  - src/logic/recovery.hpp
  - src/main.cpp
---

## Decided
src/display.cpp DMA2_Channel4_IRQHandler no longer writes USART_ICR_TCCF before setting TCIE. The DMA writes to TDR already clear TC;
if the ISR runs after the last stop bit is out, an already set TC fires the USART1 TC interrupt at once and txBusy is released.
## Tried and ruled out
Not caused by phase C (LED) changes: display.cpp was untouched. Registers when stuck: DMA2 CH4 EN=0 CNDTR=0, USART1 CR1.TCIE=1, ISR.TC=0, txBusy=1.
## Watch out
Seen on the bench when gdb halts (basetest base.call) delay the DMA TC ISR >2 byte times (~17 ms at 1200 Bd); test_display
test_rx_off_for_the_whole_frame / test_quiet_bus_holds_tx then saw no frames. Any long ISR latency in production would hang the display the same way.
