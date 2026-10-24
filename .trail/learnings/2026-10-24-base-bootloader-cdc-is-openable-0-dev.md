---
title: Base bootloader CDC is openable ~0.2 s after it shows in sysfs; the PC updater opens once and fails
author: Dev
date: 2026-10-24
branch: bl-robust
cost: { minutes: 42, tokens: 314983 }
id: l-cc33b571
scope: repo
status: active
src: agent
touches:
  - tools/basetest/bootfi.py
  - tools/basetest/fwclient.py
  - tools/basetest/glitch/.gitignore
  - tools/basetest/glitch/glitch.c
  - tools/basetest/glitch/glitch.ld
  - tools/basetest/test_bootfi_offline.py
  - docs/superpowers/plans/2026-10-24-bootloader-robustness-tests.md
  - tools/basetest/test_boot_base.py
  - tools/basetest/test_boot_remote.py
---

## Decided
- Hardware tests that talk to the base bootloader (abcd:1235) use tools/basetest/fwclient.py `bootloader_port()`, which retries the open until it succeeds (up to 10 s).
- The PC updater (midicontrolpy-sw firmware_update.cpp `updateBase`) is used only for recovery, when the base is already in the bootloader at start. That path works.

## Watch out
- Bench 2026-10-24, in the dev container: after ENTER_BOOTLOADER, `/sys/class/tty/ttyACM*` appears about 185 ms before the `/dev` node, and the node first shows up as root/0660 (Permission denied) for about 10 ms. `updateBase` finds the port through sysfs and opens it once, so `update base` failed 3 out of 3 times with "cannot open bootloader port".
- On a real host the same race exists between devtmpfs and udev setting the group. The fix belongs in the PC updater: retry `reconnect(blPort)` for a few seconds.
- fwclient: FW_UPDATE_DATA gets a FW_UPDATE_STATUS only on failure. Waiting 0.5 s for a status after every chunk made a 67 KB transfer take more than 4 min instead of 32 s.
