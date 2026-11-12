---
title: "The base log CDC replays the previous boot and can start mid-line: hw tests must skip both before matching"
author: Dev
date: 2026-11-12
branch: main
cost: { minutes: 14, tokens: 115313 }
id: l-a9060266
scope: repo
status: active
src: agent
touches:
  - tools/basetest/test_web_update.py
---

## Decided
- `tools/basetest/test_log.boot_section(log)` skips the "log before reset" block the log CDC replays when it opens, so a `wait_for` cannot match the previous boot's lines (test_ble_boot.py first matched the old "stream open" and timed out on "remote alive").
- `loglink.LogLink` drops the first partial line after an open (`_synced`): the ring replay can start in the middle of a line.
- `test_log::test_positions_survive_uint32_wrap` moves the ring HEAD to 0xFFFFFFFF-64, not the tail. Tail near 2^32 puts the masked head past the wrap (2^32 % 1536 = 1024 bytes jump), a state the firmware never reaches because it rebases once tail >= 2^31; that made the base send line stubs (FAULTS 2.4).

## Watch out
- Once in 4 runs the session teardown saw stray CDC bytes `00 d0 00 00 00 bf 04` on the protocol port after test_ble_boot; it did not reproduce with debug prints. Unexplained, harness side suspected.
