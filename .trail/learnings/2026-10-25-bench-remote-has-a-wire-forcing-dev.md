---
title: Bench remote has a wire forcing power on; the needs_user remote tests need it removed
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 5, tokens: 27488 }
id: l-66db7bba
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - src/menu.cpp
  - tools/screens/main.cpp
  - tools/basetest/test_remote_diag.py
---

## Decided
The bench remote can carry a wire that holds its power on. The remote then survives every reset, so the "remote reset = switch-off" premise of tools/basetest/test_remote_diag.py (test_remote_fault_survives_power_off, test_remote_loop_stall, test_remote_iwdg) does not hold. Remove the wire for those tests and put it back afterwards.

## Tried and ruled out
Running them with the wire in place: all 3 failed in 30 s. No fault record was reloaded from a slot before BOOT, because the RAM diag ring survived the reset and kept the records in sequence. The "off" time came from the base losing BLE (9.4 s) instead of the probe losing the core.

## Watch out
Without the wire, any remote reset switches it off, including a probe reset after flashing (openocd "program ... reset"). Someone must switch it on again. If the diag ring shows records continuing across a BOOT, the remote stayed powered.
