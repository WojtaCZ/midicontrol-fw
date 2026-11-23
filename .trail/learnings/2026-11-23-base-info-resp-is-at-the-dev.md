---
title: Base INFO_RESP is at the 128-byte payload limit; a fault record pushes link tags out
author: Dev
date: 2026-11-23
branch: main
cost: { minutes: 1, tokens: 37407 }
id: l-278fdc3b
scope: repo
status: active
src: agent
touches:
  - src/router.cpp
---

## Watch out
- `buildInfo()` in src/router.cpp: the TLV `Writer` silently drops a whole field that does not fit 128 B (MAX_PAYLOAD_SIZE). Link tags (0x14/0x15/0x17) are written last, so they are the first to go.
- FAULT_LAST (7 B) appears only while a fault record is saved, so the overflow shows only on a base that has faulted (e.g. after basetest fault injection): test_link_stats::test_counters_have_the_documented_layout fails with KeyError 23 at 131 B (bench 2026-11-23).
- Branch names longer than `main` + 1 char can still push LINK_COUNTERS out on dev builds, because the payload is 127 B with `main`. Any new base INFO tag needs room taken elsewhere.
## Decided
- BUILT stays UTC (git.cmake uses date -u) but without the " UTC" suffix, because those 4 bytes were exactly what pushed the payload to 131 B; the PC only shows the string, so nothing parses the suffix.
