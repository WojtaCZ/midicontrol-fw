---
title: "DIN OUT uses running status behind MIDI_RUNNING_STATUS; the 'stray' all-notes-off on the bench is the base's own PC-loss cleanup"
author: Dev
date: 2026-11-22
branch: main
cost: { minutes: 5, tokens: 114572 }
id: l-550fe6a2
scope: repo
status: active
src: agent
touches:
  - src/logic/running_status.hpp
  - test/host/test_running_status.cpp
  - src/midi.cpp
  - TODO.md
  - tools/basetest/test_midi.py
---

## Decided
- Owner 2026-11-22: lowest latency matters most, and the 31250 Bd DIN wire is the bottleneck (USB->DIN start ~50 us; a 4-note chord took 3.84 ms on the wire). So `midi::send` (src/midi.cpp) encodes running status via src/logic/running_status.hpp: a repeated status is left out, Note Off becomes 9n kk 00, and the status is sent again after 300 ms (the receiver recovers from a lost byte) and after SysEx or system common.
- CMake option MIDI_RUNNING_STATUS (default ON, macro MC_MIDI_RUNNING_STATUS) is the revert switch in case the organ misreads it. Not checked on the real organ yet (TODO.md).
## Tried and ruled out
- Realtime priority, draining before the other tasks, and DMA for the DIN send: the loop is at most 0.39 ms per pass without the send, so none of them would move notes meaningfully. Only shortening the bytes on the wire helps.
## Watch out
- Bench tests that compare DIN OUT bytes must accept 9n kk 00 for Note Off (tools/basetest/test_midi.py) and decode 2-byte messages (test_midi_timing.py parse()).
- The "stray" B0-BF 7B/78 burst seen in test_midi.py is not a foreign sender. It is the base's own SYS-PC-03 cleanup (router.cpp logChanges) when the PC goes away, for example after `docker stop midicontrol` before a bench run. The tests filter it out.
