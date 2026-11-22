---
title: "Base song list: a Down page must never drop the entry above the cursor; the PC page is larger than CAP"
author: Dev
date: 2026-11-21
branch: main
cost: { minutes: 12, tokens: 170061 }
id: l-a24a440d
scope: repo
status: active
src: agent
touches:
  - src/logic/song_window.hpp
  - test/host/test_song_window.cpp
---

## Decided
- songwin::Window::onResponse (src/logic/song_window.hpp) limits the Down-page drop to curIdx-1 entries and truncates the stored new codes when needed. It takes the live list selection (selNow, from menu.cpp handleSongListResponse) so Down/Up presses made while a page was in flight are kept.
## Watch out
- The PC daemon sends 25-27 codes per page, which is more than CAP=24. Before the fix a Down page dropped the whole old window, and clampAbs moved the cursor 2 entries forward on every page load (bench 2.12: 60 Downs landed on index 64).
- Host tests in test/host/test_song_window.cpp use perPage 24/25/27/40; keep a case with perPage > CAP.
- At very fast presses (30 ms) one press can be lost at the end of the loaded window while a page is pending (PREFETCH=2). This is expected, not a jump-back.
