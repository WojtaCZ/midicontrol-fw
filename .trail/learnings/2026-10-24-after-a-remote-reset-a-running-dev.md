---
title: After a remote reset a running openocd (ST-Link hla) may never read the remote again; a new openocd can
author: Dev
date: 2026-10-24
branch: bl-robust
cost: { minutes: 9, tokens: 52841 }
id: l-bd4cdd0a
scope: repo
status: active
src: agent
touches:
  - tools/basetest/bootfi.py
  - tools/basetest/test_boot_remote.py
  - docs/bootloader-robustness-report.md
---

## Decided
- tools/basetest/bootfi.py `_retry(..., restart=Board.restart_server)`: after 1 s of failed probe reads, it kills that board's openocd and starts a new one, because the old server kept answering "read_memory: failed to read memory" for more than 8 s while a fresh openocd read DHCSR at once. Without this, every remote test that resets the remote failed in where() / installed().

## Tried and ruled out
- Plain retries with sleeps (8 s) did not help: the running server never recovered.
- It is not the remote being off: the target voltage stayed at 3.21 V and a fresh openocd read the core immediately.

## Watch out
- This was seen only inside pytest runs (OTA, then reset with bp armed, then reads), never in short hand-run scripts with the same steps, so it is timing dependent. Any new remote helper that reads memory must go through `Board.u32/flash_read/core_state` (they retry and restart), not `dbg.Target.read` directly.
