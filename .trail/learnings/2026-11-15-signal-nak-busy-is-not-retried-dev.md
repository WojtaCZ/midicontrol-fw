---
title: "SIGNAL_* NAK BUSY is not retried; the remote stays stale until the next STATUS resync (up to ~5 s)"
author: Dev
date: 2026-11-15
branch: audit/step3-tests
cost: { minutes: 27, tokens: 431477 }
id: l-90cd5fe2
scope: repo
status: active
src: agent
touches:
  - src/logic/stale_link.hpp
  - test/host/test_stale_link.cpp
---

## Watch out
- src/comm.cpp: only the blocking sendReliable retries NAK BUSY (line ~551). Async reliable sends (sendWithAckImpl ->
  _pending, used for SIGNAL_PLAYING/ANCHOR/STOPPED to the remote) treat any NAK as final: noteRemoteSignalEnd sets
  _remoteDirty and the remote is resynced only on the next STATUS_RESP with ble=1 (5 s GET_STATUS cadence).
- Seen on the bench 2026-11-15 (audit/runs/2026-11-15-latency trial 29): the base NAKed SIGNAL_PLAYING + ANCHOR with
  BUSY right at "Playing"; the remote showed the playing screen 3.6 s late (normal 0.1 s). 3 SIGNAL_* BUSY NAKs in 30 trials.
- A remote frame missing a few seconds after Playing is therefore a real delay, not a rig observation gap.

## Decided
- Not fixed yet (audit step 3): proposal = retry BUSY in the async path like a timeout (as sendReliable does). Owner decides.
