---
title: "Remote learns the PC state from the base's KEEPALIVE payload [pcAlive], not by polling GET_STATUS"
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 11, tokens: 171536 }
id: l-f776dfd4
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - inc/git.hpp
  - inc/menu.hpp
  - src/menu.cpp
  - tools/screens/main.cpp
  - src/router.cpp
---

## Decided
- 2026-10-25: the remote's "Odpojeno / Cekam na PC" splash needs the PC state. The base's KEEPALIVE to the remote (src/router.cpp sendKeepaliveToRemote) carries `[pcAlive]` and is also sent at once when isPcAlive() changes; the remote keeps it in link::pcAlive() (midicontroller-fw src/link.cpp). Spec: midicontrol-protocol PROTOCOL.md 2b5fe92.
- An empty KEEPALIVE payload (older base, or the PC→base keepalive) means pcAlive = 1, so mixed versions never lock the remote behind the splash.

## Tried and ruled out
- Remote polling GET_STATUS → STATUS_RESP (already has pcAlive): 4 frames per poll (request, ACK, response, ACK) and it ties up one of the remote link's 4 pending slots during playback. Not built.

## Watch out
- The overlay splashes (base noPcScreen, remote noBaseScreen) are pushed only over the main menu and popped only when on top (sync at the start of GUI::render()); every other screen is pushed above them, which is what gives pairing/fw update/power-hold priority. Do not reset the stack to show them.
