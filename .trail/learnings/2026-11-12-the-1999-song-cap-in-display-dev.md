---
title: "The 1999 song cap in display setSong() is intended: songs go up to 1999 only"
author: Dev
date: 2026-11-12
branch: main
cost: { minutes: 7, tokens: 15602 }
id: l-45f0873d
scope: repo
status: active
src: agent
touches:
  - docs/HARDWARE.md
  - docs/SYSTEM.md
  - docs/HARDWARE-remote.md
  - src/display.cpp
---

## Decided
Owner, 2026-11-12 (D23 in docs/SYSTEM.md): the church indicator's songs go up to 1999. The thousands digit is blank or 1. The cap in src/display.cpp setSong() is correct.
## Tried and ruled out
Allowing any thousands digit (songs up to 9999): briefly recorded, then corrected by the owner the same day.
## Watch out
The V2 bus frame can carry any digit 0-9 in the thousands byte, so the protocol alone does not explain the cap. Do not "fix" it.
