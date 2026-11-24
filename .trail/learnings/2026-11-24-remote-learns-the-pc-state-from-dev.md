---
title: "Remote learns the PC state from KEEPALIVE [pcAlive]; persistent overlays stay main-menu-only, the timed error screen covers ANY screen"
author: Dev
date: 2026-11-24
branch: backlog/abort-1124
cost: { minutes: 14, tokens: 50921 }
id: l-e91c621d
scope: repo
status: active
replaces: l-f776dfd4
src: agent
touches:
  - CMakeLists.txt
  - lib/protocol
  - lib/ui
  - test/host/test_timed_splash.cpp
  - tools/screens/CMakeLists.txt
  - src/menu.cpp
---

## Decided
- 2026-11-24 (owner item 17): ONE timed error screen on both devices ("Odpojeno" layout, big word = cause, small line = what to do) is pushed over ANY screen, including the player; 5 s (ui::TimedSplash) or a key closes it. Rule in lib/ui ui::LinkErrorPolicy: NoLink -> "Odpojeno / Cekam na ..." at once; Timeout -> "Odpojeno" if a 2nd Timeout falls within 10 s, the peer is not alive, or a STOP timed out, else "Bez odpovedi / Zkus znovu"; NAK -> "Chyba / <reason>". Base: GUI::requestFailed(type, Failure, text) in src/menu.cpp (no NoLink on the base, USB).
- The persistent overlay splashes (base noPcScreen, remote noBaseScreen) are STILL pushed only over the main menu and popped only when on top; this is what gives pairing/fw update/power-hold priority. Do not reset the stack to show them. Only the timed notices (error, abort) cover everything.
- The remote's "Odpojeno / Cekam na PC" needs the PC state: the base's KEEPALIVE to the remote carries `[pcAlive]` (also sent at once on change); the remote keeps it in link::pcAlive(). An empty KEEPALIVE payload (older base, or PC->base) means pcAlive = 1 so mixed versions never lock the remote behind the splash. Spec: midicontrol-protocol PROTOCOL.md.

## Tried and ruled out
- Remote polling GET_STATUS for the PC state: 4 frames per poll and it ties up one of the 4 pending slots during playback.
- Item 17 first reading "overlay on main menu only" for the error screen: owner approved covering any screen.

## Watch out
- Notices pushed over the player/recording screen leave stale flags: GUI::render() drops a player or recording screen whose flag is false. A push at full navigator depth (8) is ignored silently.
