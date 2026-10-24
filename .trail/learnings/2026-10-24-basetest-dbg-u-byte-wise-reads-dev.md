---
title: "basetest dbg.u() byte-wise reads tear multi-byte counters; 'systick went back' in the soak is not a base reset"
author: Dev
date: 2026-10-24
branch: base-hw-test
cost: { minutes: 16, tokens: 144741 }
id: l-8ebd5478
scope: repo
status: active
src: agent
touches:
  - ".superpowers/sdd/2026-10-19-base-finish-and-hw-test/final/fix-wave.md"
  - tools/basetest/dbg.py
  - tools/basetest/README.md
---

## Watch out
tools/basetest/dbg.py `Target.read()` uses openocd `read_memory <addr> 8 <n>`, i.e. one byte access per byte. `u()` builds
4-byte values from that, so a counter the ISR increments (stmcpp::systick::ticks_, router::_lastRemoteRx) can be read torn
across a byte carry. 600 s soak 2026-10-24: "base systick went back (271871 -> 271819)" and "(324607 -> 324555)"; both
"before" values were 0x...FF, USB never re-enumerated and 600/600 keepalives were ACKed, so the base did not reset.
## Decided
Read naturally aligned 2/4-byte values with a single 16/32-bit access (`read_memory addr 32 1`). A real base reset also
drops the USB CDC, so cross-check any "reset" verdict against the link.
