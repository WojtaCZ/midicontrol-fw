---
title: "Bench remote: short resets keep it powered, only a long halt (reflash) switches it off"
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 39, tokens: 154169 }
id: l-2ae7598e
scope: repo
status: active
replaces: l-66db7bba
src: agent
touches:
  - tools/basetest/test_remote_diag.py
---

## Decided
tools/basetest/test_remote_diag.py (test_remote_fault_survives_power_off, test_remote_loop_stall, test_remote_iwdg) accepts both outcomes: the remote reset and stayed powered (detected by the boot counter in TAMP->BKP1R, records between the previous and the newest BOOT in the RAM ring), or it switched off (the probe loses the target, a person switches it on).

## Tried and ruled out
Removing the wire that holds the remote on, so these tests switch it off: the remote still stayed on. A HardFault, loop-stall or IWDG reset is over before the power latch (PA1) drops. USB-C does not keep it on: an openocd "program ... reset" halts the core for seconds, and that did switch it off with the wire out (bench 2026-10-25, battery disconnected, powered from USB).

## Watch out
With the wire out, reflashing the remote through the probe switches it off; someone must press ON/OFF. The slot path after a real power loss (records reloaded from the W25Q32 before BOOT) is not exercised by a short reset. A warm boot also reloads unfetched slots oldest first, so with no PC software fetching them, a backlog hides a new fault for one or more boots. Erase the diag sector (boot::w25q::eraseSector(0x3FF000) through gdb) before these tests.
