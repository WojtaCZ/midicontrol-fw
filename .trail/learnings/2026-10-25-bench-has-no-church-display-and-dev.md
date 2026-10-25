---
title: Bench has no church display and an LED instead of the SSR relay
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 1, tokens: 0 }
id: l-e25f153e
scope: repo
status: active
src: agent
touches:
  - src/display.cpp
  - docs/bench-checklist.md
  - tools/basetest/test_display.py
---

## Decided
The bench base has no church display connected (the PC13 sense in src/display.cpp stays high), and the SSR output drives an LED, not a relay. Check "Napajeni varhan" by watching the LED. Check the display hot-plug in docs/bench-checklist.md by pulling the PC13 sense low by hand, or skip it.

## Watch out
display::connected can read 1 while PC13 reads 1 (src/display.cpp sets connected = !disp_sense.read()). tools/basetest/test_display.py forces display::connected = 1 through gdb, and nothing clears it until a PC13 edge or a reset. Reset the base, or toggle PC13, before you trust that flag.
