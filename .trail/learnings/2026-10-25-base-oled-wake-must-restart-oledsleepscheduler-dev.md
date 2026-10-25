---
title: Base OLED wake must restart oledSleepScheduler; pause/resume never re-arms a fired one-shot
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 1, tokens: 10273 }
id: l-42c01959
scope: repo
status: active
src: agent
touches:
  - src/main.cpp
  - src/menu.cpp
  - src/oled.cpp
---

## Decided
Oled::wakeupCallback restarts oledSleepScheduler (scheduler.start()), as the remote does. Events that must light the display (pairing result, now playing/stop/anchor, fw progress) call Oled::wakeupCallback in src/menu.cpp. Oled::sleep sends 0xAE through a sleepPending chunk in Oled::process; wakeup re-sends the frame first and then 0xAF (src/oled.cpp).

## Tried and ruled out
The old pause()/resume() on wake: once the 30 s one-shot has fired, resume() does not arm it again, so the display never went back to sleep. Before c880b14 oledSleepScheduler was not even dispatched in the main loop (src/main.cpp).

## Watch out
Oled::flush() must not wait for a frame while the display sleeps (framePending && !isSleeping()). test_fault.py calls flush through gdb.
