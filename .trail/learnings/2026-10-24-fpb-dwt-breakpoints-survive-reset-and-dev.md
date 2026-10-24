---
title: FPB/DWT breakpoints survive reset and openocd exit; a stale one HardFaults the core once debug is released
author: Dev
date: 2026-10-24
branch: bl-robust
cost: { minutes: 1, tokens: 2098 }
id: l-ab0967df
scope: repo
status: active
src: agent
touches:
  - tools/basetest/bootfi.py
---

## Decided
- tools/basetest/bootfi.py `Board.disarm_all()` clears FP_COMPn and DWT_FUNCTIONn by direct register writes, because FPB/DWT are reset only by POR and openocd 0.12 has no `rwp all`. Otherwise a breakpoint left behind HardFaults the core later.
- `Board.arm` sets watchpoints only while the core is halted, because with hla (ST-Link) a `wp` set while the core runs is accepted but never fires (watchpoint on a RAM tick counter: no halt in 3 s while running, immediate halt when set while halted).
- Resume goes through `dbg.Target.resume()` (halt + resume), because openocd ignores `resume` until it has polled the halt; the core then stays halted at the same pc. Registers are read with `reg <r> force` because `get_reg`/`reg` returned cached values from an earlier halt.

## Watch out
- After an aborted script, the base bootloader was found in Default_Handler with IPSR 3: a stale FPB breakpoint hit after C_DEBUGEN was released becomes a HardFault.
- A probe `reset run` (reset_config none) lands a few µs after the core resumes. A breakpoint "before" a flash operation does not reset before it, because the erase or program has often started already. That is how the page0-commit ECC brick (F4) was first hit.
