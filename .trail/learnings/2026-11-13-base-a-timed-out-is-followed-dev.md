---
title: "Base: a timed-out $$$ is followed by ---; the remote deliberately is not"
author: Dev
date: 2026-11-13
branch: main
cost: { minutes: 4, tokens: 21382 }
id: l-7c4e569d
scope: repo
status: active
src: agent
touches:
  - src/bluetooth.cpp
---

## Decided
2026-11-13: base src/bluetooth.cpp ENTER_CMD timeout sends "\r---\r" and goes to EXIT_CMD. A module that enters command mode
after the 1 s timeout would otherwise keep swallowing frames as commands (robustness report F6, inferred from code and traces).
Bench: the base BLE regression and the remote ECC test pass with it.
## Tried and ruled out
The same change on the remote (midicontroller-fw) broke the BLE regression on every combined run and was dropped. See that repo's learning.
## Watch out
The base wedge seen while testing this ("$$$ timeout" every 60 s) was NOT caused by the --- change. It was the status-parser flag
(see the base-rn4870-wedge learning, logic/status_framer).
