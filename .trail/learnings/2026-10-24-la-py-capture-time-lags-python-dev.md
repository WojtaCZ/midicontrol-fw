---
title: "la.py capture time lags Python's clock by ~0.5 s; time windows in analyzer tests need a margin"
author: Dev
date: 2026-10-24
branch: base-hw-test
cost: { minutes: 2, tokens: 13171 }
id: l-8c3e128b
scope: repo
status: active
src: agent
touches:
  - tools/basetest/test_ble_security.py
  - CLAUDE.md
  - src/main.cpp
  - lib/protocol
  - tools/basetest/test_pairing.py
  - tools/basetest/test_remote_diag.py
  - tools/basetest/README.md
  - tools/basetest/dbg.py
  - tools/basetest/test_dbg_offline.py
  - tools/basetest/test_soak.py
---

## Watch out
tools/basetest/la.py: `la.start()` launches sigrok, and the capture's t=0 comes a few hundred ms after the Python call.
Timestamps from `la.uart_timed()` are therefore earlier than the matching `time.sleep()` schedule in the test. In
test_ble_security.py::test_no_reconnect_without_stored_remote, the reconnect `C` triggered by restoring the address at
Python ~13.0 s appeared at capture 12.98 s, inside the "no C before 13.0" window, and failed a correct firmware (bench
2026-10-24, base 8727d5a).
## Decided
Windows bounded by an action done from Python end ~1 s before the action's nominal time (commit 8974344: [2.5, 12.0)
negative, [12.0, 21.0) positive). Print the event timestamps before blaming the firmware.
