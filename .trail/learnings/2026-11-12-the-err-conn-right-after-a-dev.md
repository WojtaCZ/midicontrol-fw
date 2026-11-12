---
title: "The ERR_CONN right after a base boot is the remote restarting advertising, not a lost link"
author: Dev
date: 2026-11-12
branch: main
cost: { minutes: 1, tokens: 11318 }
id: l-0943477b
scope: repo
status: active
src: agent
touches:
  - src/logic/link_stats.cpp
---

## Decided
- FAULTS 2.2 / SYS-PWR-02: after a base reset the RNBD350 reports %CONNECT% then %ERR_CONN% (~120 ms) on the first CE (~5.0 s), before %STREAM_OPEN%; the module connects again by itself ~200 ms later and the stream opens ~7.4 s. The link always came up on the first CE, only link_losses/link_errconn started at 1.
- Cause is on the remote: it still holds the link to the base that just reset, loses it ~2 s into the boot (remote diag 18 BLE_ERR_CONN), and handleBleEvent runs scheduleAdvertising(1000 ms) -> blocking $$$ IA/A command sequence that overlaps the base's first connection setup. Remote diag after a base boot shows only one CONNECT (the base's second).
- Fix: src/logic/link_stats.cpp `established` flag (set by onStreamOpen, cleared by onConnect); onLost returns false and counts nothing before STREAM_OPEN. Same file in midicontroller-fw (keep them identical). main.cpp logs "connect failed during setup".
## Tried and ruled out
- Logic analyzer capture of the BLE UARTs: base channels empty, remote channels garbage (mapping in la.py/HARDWARE.md is suspect), so the collision is inferred, not seen on the UART.
- Reconnects after a remote module reset (no base boot) never show it: boot-specific.
## Watch out
- Proof experiment: hold the remote BLE reset through the base boot and release 1 s later -> 0/3 setup failures; normal boot -> 3/3 (scratch bootexp.py).
- Test: tools/basetest/test_ble_boot.py must skip the replayed "log before reset" section (boot_section) or it matches the previous boot's "stream open".
