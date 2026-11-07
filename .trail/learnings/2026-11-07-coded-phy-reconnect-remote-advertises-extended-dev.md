---
title: "Coded-PHY reconnect: remote advertises extended on Coded; base uses FE/CE; a link born with a new bond over Coded goes silent"
author: Dev
date: 2026-11-07
branch: main
cost: { minutes: 30, tokens: 175204 }
id: l-0f419c43
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - TODO.md
  - inc/bluetooth.hpp
  - src/bluetooth.cpp
  - src/main.cpp
---

## Decided
Remote (midicontroller-fw/src/bluetooth.cpp startAdvertising) runs extended connectable advertising on Coded primary+secondary (IPE,0001,0200,07,0F,03,03 + IAE + A,...,01,01); Bluetooth::extAdvMode=0 gives legacy. Base (midicontrol-fw/src/bluetooth.cpp) scans with FE,01E0,0190,03,00 and connects with CE,<type>,<addr>,05,<1M set>,<Coded set> for both reconnect and pairing. The extended set uses the same public address, so the existing bond works; B still follows %STREAM_OPEN%. pairingFinished(true) in main.cpp drops the link once (K,1) and lets auto-reconnect secure it.

## Tried and ruled out
Plain C against a Coded advertiser: never connects (bench). Legacy and extended sets both connectable: not allowed, the second set must be a beacon. 96-byte job slots for the 81-char CE line: RAM went to 97%, so the line is built into _ceLine at send time (token @CE). Staying on the link made by a fresh pairing over Coded: module link stays up but no frames flow either way (remote mode DATA, rxBytes grow slowly); same pairing with legacy advertising is fine and so is every reconnect. Cause not found.

## Watch out
Remote is invisible to phones and old bases (legacy off). Real range is unverified (church walk). Base module wedges ($$$ timeout) after gdb halts from bench scripts or right after flashing: reset the base and wait ~20 s.
