---
title: "Pairing: the scan-keeps-cmd-mode check must match FE, not only F"
author: Dev
date: 2026-11-22
branch: main
cost: { minutes: 15, tokens: 121151 }
id: l-af746c88
scope: repo
status: active
src: agent
touches:
  - TODO.md
  - tools/basetest/dbg.py
  - src/logic/ble_status.hpp
---

## Decided
The base BLE engine stays in command mode after a scan command only while the scan runs; "scan command" must match both `F` and `FE[,...]` (blestatus::isScanCommand in src/logic/ble_status.hpp). Since 6a68acc pairing sends `FE,01E0,0190,03,00`; the old strcmp(lastCmd,"F") check missed it, the engine sent `---` into the running scan and every later `$$$` timed out (pairing "nenalezen"/timeout).
## Watch out
Any new scan variant (other F-prefixed commands) must go through isScanCommand. After PAIR_DONE the link is alive only ~6 s later (drop + reconnect on a new bond); test timeouts below ~10 s flake.
