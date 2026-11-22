---
title: "basetest LA assertions hardcode RNBD350 commands; 6a68acc's FE/CE switch left them stale"
author: Dev
date: 2026-11-21
branch: main
cost: { minutes: 1, tokens: 36564 }
id: l-2ceff8b4
scope: repo
status: active
src: agent
touches:
  - tools/basetest/test_ble_security.py
  - tools/basetest/test_pairing.py
  - tools/basetest/test_ble_down.py
  - src/bluetooth.cpp
  - src/main.cpp
---

## Watch out
- `tools/basetest/test_ble_security.py`, `tools/basetest/test_pairing.py` and `tools/basetest/test_ble_down.py` assert on the base's UART text captured by the logic analyser ("F\r", "C,0,<addr>", "RN4870 init failed"). Commit 6a68acc (Coded PHY) switched `src/bluetooth.cpp` to "FE,..." and "CE,<type>,<addr>,...", and `src/main.cpp` now logs "RN4870 <init|configure> failed". The hw tests were not updated because they only run on the bench, so bench step 5.1 reported 4 failures that were not hardware faults.
## Decided
- 2026-11-21: the tests accept the old and new forms through regexes (CE?,0,<addr>[,\r]; (init|configure)). The offline suite cannot catch this kind of breakage. When you change a command string in `src/bluetooth.cpp` or `src/main.cpp`, grep tools/basetest for it.
