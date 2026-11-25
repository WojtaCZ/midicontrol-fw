---
title: "Base and remote twin screens must match (icons, layout, delays)"
author: Dev
date: 2026-11-25
branch: main
cost: { minutes: 7, tokens: 200733 }
id: l-1ec1fee0
scope: repo
status: active
src: agent
touches:
  - src/menu.cpp
---

## Decided
- Screens that exist on both the base (midicontrol-fw/src/menu.cpp) and the remote (midicontroller-fw/src/*_screen.cpp) must look and behave the same: same icons (no text where the other side has an icon), same layout, same timings.
- Example fixed 2026-11-25: the base NowPlaying used text for "Pridat sloku"; it now uses the remote player_screen.cpp Stop|Repeat icon pair. The song list "Nacitani..." screen appears only after 1 s on both (base songListDelayScheduler 1000_ms, remote LOADING_SHOW_MS).

## Watch out
- When you change one side's screen, check its twin and the SYSTEM.md requirement (SYS-USE-*) in the same change. The owner flags any mismatch at the bench.
- The sim flows in both repos' tools/screens/flows encode the timings, so update them together.
