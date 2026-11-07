---
title: "RNBD350 reports a vanished peer as %ERR_CONN%, never %DISCONNECT%; wrong BOOT0 looks like a dead BLE init"
author: Dev
date: 2026-11-07
branch: main
cost: { minutes: 1, tokens: 23503 }
id: l-4d9f3386
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - CMakeLists.txt
  - TODO.md
  - inc/midi.hpp
  - src/logic/recovery.cpp
  - src/logic/recovery.hpp
  - src/main.cpp
  - src/midi.cpp
  - test/host/test_recovery.cpp
  - tools/basetest/README.md
---

## Decided
2026-11-07 bench: with the remote's module held in reset (or the remote powered off) the base RNBD350 emits only `%ERR_CONN%` (status type 35), never `%DISCONNECT%`. src/main.cpp now treats ERR_CONN while `Bluetooth::isConnected()` as link lost (~1 s, relink ~2 s after the remote returns); ERR_CONN while not connected is only a failed attempt. Test: tools/basetest/test_link_loss.py. The remote's 20 min auto-off (midicontroller-fw) therefore looks to the base exactly like this.
Auto-off never fires while VBUS is present (bench remote sits on USB/probe); `base::Power::autoOffDisabled` is set by the basetest `remote` fixture, otherwise a 2 h suite powers the remote off after 20 min and ~10 remote-dependent tests fail for no real reason.

## Tried and ruled out
Base "RN4870 not responding / $$$ timeout" right after flashing: caused by BOOT0 being in the wrong position (ROM bootloader talks on USART2 to the module), not by firmware; 20/20 base resets afterwards linked fine. Do not change option bytes for it (I briefly set nSWBOOT0=0 and restored 0xFFEFF8AA).

## Watch out
Base ST-Link is now the V3 (serial 004200443335510C35383531), not 39FF71...; remote is the ST-LINK/V2. `pkill -f "adapter serial ..."` kills your own shell. Logic analyzer was absent: LA tests fail with "No devices found". test_set_current_on_off uses a stale menu index; test_log wrap failure not analysed.
