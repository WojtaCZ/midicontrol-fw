---
title: "basetest stream gaps: a link-loss panic burst is not a stall"
author: Dev
date: 2026-11-23
branch: main
cost: { minutes: 19, tokens: 313078 }
id: l-0c442894
scope: repo
status: active
src: agent
touches:
  - inc/looptime.hpp
  - tools/basetest/test_midi.py
---

## Tried and ruled out
test_midi_latency_during_reconnect showed one ~200 ms "gap" while the remote BLE was held in reset. looptime (inc/looptime.hpp via dbg loop_reset/loop_stats) showed no loop pass over ~1.1 ms and no section over 1 ms, so the base never blocked.

## Decided
The base sends a panic burst (CC 123/120 on all channels) when the link drops. It arrives before the 0.2 s settle in stream() (tools/basetest/test_midi.py) and was counted as the first inter-arrival gap. check_stream measures gaps between note-ons only.

## Watch out
OTA result screens ("Chyba aktualizace"/"Instalace", fwProgressScreen) stay up kResultMs = 10 s and swallow keys; dbg.goto_player waits them out. A test that runs right after an OTA test otherwise fails to navigate.
