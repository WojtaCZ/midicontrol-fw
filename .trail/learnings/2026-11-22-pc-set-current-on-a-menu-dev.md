---
title: "PC SET_CURRENT on a menu-switched organ re-marks the power source as PC (R11, owner kept)"
author: Dev
date: 2026-11-22
branch: main
cost: { minutes: 5, tokens: 35227 }
id: l-1546afca
scope: repo
status: active
src: agent
touches:
  - src/router.cpp
---

## Decided
Owner 2026-11-22 (bench 2.6c): keep ruling R11 in src/router.cpp: the last writer owns the organ power source.
If the organ was switched on from the base menu (organ_src=2) and the PC later sends SET_CURRENT 1,
the source becomes PC (organ_src=1).

## Tried and ruled out
"Manual wins until off" and "decide later with B1/B3": owner chose to keep last-writer-owns.

## Watch out
Accepted cost: the PC idle switch-off (midicontrol-sw TODO B1/B3, armIdleOff in src/comm.cpp) may turn
off an organ the organist switched on by hand once the PC has sent its own power-on. Do not "fix" this.
