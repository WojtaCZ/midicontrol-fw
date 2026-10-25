---
title: "Probe reads fail while the remote bootloader erases flash, and the hla recovery clears FPB/DWT: late bootloader breakpoints must be re-armed"
author: Dev
date: 2026-10-25
branch: bl-robust
cost: { minutes: 9, tokens: 23477 }
id: l-1ff01355
scope: repo
status: active
replaces: l-de061de1
src: agent
touches:
  - tools/basetest/bootfi.py
  - tools/basetest/test_boot_remote.py
---

## Decided
- tools/basetest/bootfi.py `Board.wait_halt()` lives with failed DHCSR reads instead of restarting openocd. After the first good read that follows a failure, it re-arms every bp/wp in `Board._armed`, because FP_COMP0 read 0 right after the read errors (trace via BOOTFI_TRACE). With this, copyRange watchpoints, clearStagePending and w25q::writeData all hit (2/2), where before they missed 3/3.
- tools/basetest/test_boot_remote.py `stage_halted_at_reboot()`: the app is stopped at fwupdate::reboot() and openocd does the reset itself, so the bp is armed with the core halted and openocd stays attached.

## Tried and ruled out
- Restarting openocd on read errors (`_retry(restart=...)`): the new openocd clears FPB/DWT at examine, so the bp is lost.
- Re-arming only after an openocd restart, and raw FP_COMP writes with openocd exited: openocd removes its own bps on a normal exit.

## Watch out
- While the remote bootloader erases the internal app region (55 pages, ~1.2 s), AHB-AP reads return "read_memory: failed to read memory". Breakpoints that fire before the stall ends (erasePageAt, the first copyRange) worked even without the re-arm, which hid the problem.
- `halt; resume` (dbg.Target.resume) after editing pc/r0 drops the edit: use `resume <addr>`.
