---
title: "basetest dbg.call: 'Error setting register' after the call is gdb detach on a resumed core, not a failed call"
author: Dev
date: 2026-11-22
branch: main
cost: { minutes: 9, tokens: 108889 }
id: l-37567339
scope: repo
status: active
src: agent
touches:
  - tools/basetest/dbg.py
---

## Watch out
tools/basetest/dbg.py _gdb() ends with "monitor resume" then "detach". After an inferior call gdb writes cached registers back on detach; the core already runs, openocd refuses -> "Error setting register rN/dN/msp/lr" (random register). Seen 2026-11-22 with arm-none-eabi-gdb 17.2 and system gdb 18.1 alike, so not the gdb-common/python mismatch warning. The call has returned ($1 = ... printed). call() now ignores that line.
## Tried and ruled out
Pointing gdb at an empty --data-directory or using /usr/sbin/gdb: the error stays (intermittent).
