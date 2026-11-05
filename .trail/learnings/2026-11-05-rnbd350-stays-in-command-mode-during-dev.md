---
title: "RNBD350 stays in command mode during a scan: no --- or $$$ answer, so F must not exit command mode"
author: Dev
date: 2026-11-05
branch: rnbd350
cost: { minutes: 10, tokens: 51104 }
id: l-9d986c4b
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - src/bluetooth.cpp
---

## Decided
After a successful F, the command engine in src/bluetooth.cpp (WAIT_RESP) keeps command mode instead of sending "---". The X and C queued on "remote found" then go out in the same session. Bench 2026-11-05: pairing works end to end (found, X, C, connect, stream open, B, bonded, saved, remote alive).

## Tried and ruled out
Scan-line parsing alone (0bcbe54) was not enough: the remote was found but pairing failed in state 2 (CONNECTING) with "$$$ timeout".

## Watch out
Evidence: "exit cmd: timeout" (no END for "---") while scan adverts kept arriving, then "$$$" got no "CMD> ". The engine believed it was in DATA mode while the module was still in command mode. Any new job started while a scan runs must assume command mode.
