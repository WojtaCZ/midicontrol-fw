---
title: "Base RN4870 wedge: USART2 status flag stuck after a lost '%', fixed with logic/status_framer"
author: Dev
date: 2026-11-13
branch: main
cost: { minutes: 13, tokens: 21374 }
id: l-9584565e
scope: repo
status: active
src: agent
---

## Decided
The base split `%...%` statuses in the USART2 ISR with a plain `_receivingStatus` flag. One lost or extra `%` left it set for good, so `CMD> ` after `$$$` was swallowed as status text, every `$$$` timed out ("RN4870 init failed again" every 60 s) and the RST restart did not help, because `%REBOOT%` only flips the flag. Confirmed on the bench 2026-11-13 after the remote ECC test: `_receivingStatus` read true over SWD for 12 s with no link; only an MCU reset recovered it. The bond was fine, so re-pairing is not the fix.
Fix: `src/logic/status_framer.{hpp,cpp}` (host tests `test/host/test_status_framer.cpp`) resyncs on a control byte or a >50 ms pause inside a status. `restart()` calls `reset()`.

## Watch out
Unlike the remote's framer (midicontroller-fw, TEXT_LEN 48, resyncs on over-long text), the base truncates long statuses at 95 chars and does not abandon them, because the scan results from `F` are longer than the buffer.
A raw `%` never appears inside a protocol frame (PROTOCOL.md escapes 0x25), so the framer cannot eat frame bytes.
If "$$$ timeout" repeats while the bond is intact, read `_statusFramer` (`active_`, `resyncs_`) over SWD before you suspect the module.
