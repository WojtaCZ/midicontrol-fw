---
title: "Intermittent: base RN4870 ignored $$$ after the remote's radio was held in reset (test_pairing_timeout_no_remote)"
author: Dev
date: 2026-10-24
branch: base-hw-test
cost: { minutes: 1, tokens: 3367 }
id: l-7382acfb
scope: repo
status: active
replaces: l-d4b12895
src: agent
touches:
  - src/bluetooth.cpp
---

## Watch out
Bench 2026-10-24, base 251285b: test_pairing.py::test_pairing_timeout_no_remote failed twice in a row at
disconnect_and_forget ("timeout 10 s: base disconnected"), and every later remote_away / remote-alive fixture errored.
Capture of a scripted repro in that state: the base sent `$$$` 0.6 s after the remote's RN4870 was put in reset; the base
module never answered `CMD> `, so U,Z/K,1 never went out, and `%DISCONNECT%` never came for 16 s. The engine's
"$$$ timeout" (CMD_TIMEOUT 1 s, src/bluetooth.cpp) returned it to data mode, but the module's later `CMD> ` replies to
the keepalive frames show the module itself had gone into command mode late: module and firmware disagreed on the mode.
## Tried and ruled out
Not the new secure watchdog or the build: after flashing 8727d5a, re-pairing and flashing 251285b back, the same scripted
repro passed on both builds, and the full suites passed (pairing 3/3, ble_security 7/7, relay, ble_down). The cause was
not found; the trigger state was lost when U,Z reset the bond tables.
## Decided
If it shows up again, capture D2/D3 (la.py) and the base log ("$$$ timeout" W lines, wedge restart) before touching
anything: don't run U,Z or re-pair first, because that clears the state. Check whether the wedge detector (_dollarTimeouts)
fires when the module answers data with `CMD> ` but never answers `$$$`.
