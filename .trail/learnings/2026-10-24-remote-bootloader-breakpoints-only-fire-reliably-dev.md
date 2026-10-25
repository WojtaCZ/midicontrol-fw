---
title: "Remote bootloader breakpoints only fire reliably when openocd itself does the reset: stop the app at fwupdate::reboot() first"
author: Dev
date: 2026-10-24
branch: bl-robust
cost: { minutes: 18, tokens: 76952 }
id: l-de061de1
scope: repo
status: active
src: agent
touches:
  - tools/basetest/test_boot_remote.py
---

## Decided
- tools/basetest/test_boot_remote.py `stage_halted_at_reboot()` + `boot_bootloader()`: stage the OTA to END, stop the app at fwupdate::reboot() (app bp, no reset yet), arm the bootloader bp while halted, then `reset run` from openocd. That way openocd keeps the core across the reset and its breakpoints are restored, because after the remote's own NVIC_SystemReset the running hla openocd loses the core and a new openocd clears FPB/DWT at examine.

## Tried and ruled out
- Arming the bootloader bp in the running app and letting the app reset itself: late bps (copyRange watchpoints, clearStagePending, w25q::writeData) missed 3/3 runs. Early ones (erasePageAt) hit, because they fire before openocd has to reconnect.
- Re-arming after an openocd restart: still missed when the bp fires in the restart window.
- Writing FP_COMP directly and leaving openocd: openocd removes its own bps on exit, and the raw comparator read back as 0 via probe-rs (probe-rs serial is 37C3BF70..., not 37FF70...).

## Watch out
- `halt; resume` (dbg.Target.resume) after editing pc/r0 discards the edit: use `resume <addr>` directly when forcing a function return.
