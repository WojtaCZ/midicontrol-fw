---
title: "Intermittent: base RN4870 ignored $$$ after the remote's radio was held in reset (test_pairing_timeout_no_remote)"
author: Dev
date: 2026-10-24
branch: base-hw-test
cost: { minutes: 28, tokens: 53319 }
id: l-d4b12895
scope: repo
status: active
src: agent
touches:
  - tools/basetest/test_ble_security.py
---

## Watch out
Bench 2026-10-24, base 251285b: test_pairing.py::test_pairing_timeout_no_remote failed twice in a row at
disconnect_and_forget ("timeout 10 s: base disconnected"), and every later remote_away / remote-alive fixture errored.
Capture of a scripted repro in that state: the base sent `$$$` 0.6 s after the remote's RN4870 was put in reset; the base
module never answered `CMD> `, U,Z/K,1 never went out, `%DISCONNECT%` never came for 16 s, and the base's keepalive frames
then got `CMD> ` replies (module in command mode while the firmware thought it was in data mode).
## Tried and ruled out
Not the new secure watchdog or the build: after flashing 8727d5a, re-pairing and flashing 251285b back, the same scripted
repro passed on both builds, and the full suites passed (pairing 3/3, ble_security 7/7, relay, ble_down). The cause was
not found; the trigger state was lost when the bond tables were reset by U,Z.
## Decided
If it shows up again, capture D2/D3 (la.py) before touching anything: don't run U,Z or re-pair first, because that clears
the state. The firmware has no recovery when `$$$` gets no `CMD> ` prompt: worth a look in bluetooth.cpp's command engine.
