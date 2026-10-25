---
title: "Without a stored remote the no-PC overlay's comment carries the Sparuj ovladac hint"
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 21, tokens: 109929 }
id: l-9b19ce5a
scope: repo
status: active
src: agent
touches:
  - src/menu.cpp
  - tools/screens/main.cpp
  - src/main.cpp
---

## Decided
The user chose (2026-10-25): with no stored remote, the "Odpojeno" overlay (noPcScreen, syncNoPcScreen in src/menu.cpp) shows "Sparuj ovladac" instead of "Cekam na PC", and a press still opens Nastaveni. The main-menu title hint stays for when a PC is connected. Screen scenario 40_no_pc_unpaired in tools/screens/main.cpp.

## Tried and ruled out
Keeping only the title hint: noPcScreen covers the main menu whenever no PC is connected, so the hint was never visible on a bench or base without a PC. A separate pairing screen above the overlay (OK starts pairing): not chosen, it would be one more screen.

## Watch out
SplashScreen::setComment marks the screen dirty and restarts the scroll, so syncNoPcScreen (it runs on every render) calls it only when the text changes.
