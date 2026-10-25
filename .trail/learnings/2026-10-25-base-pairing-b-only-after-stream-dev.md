---
title: "Base pairing: B only after STREAM_OPEN, and one reconnect when the remote still holds an old bond"
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 1, tokens: 2137 }
id: l-2805bd15
scope: repo
status: active
src: agent
touches:
  - src/main.cpp
  - tools/basetest/test_pairing.py
---

## Decided
Pairing in src/main.cpp: %CONNECT% → BONDING with _secureOnStreamOpen; B is sent on %STREAM_OPEN%, as on reconnect. When the link drops during CONNECTING/BONDING, the base connects once more (_pairRetried, connectTo with _pairAddrType) before it reports FAILED.

## Tried and ruled out
B straight after %CONNECT%: the link drops whenever B runs before %STREAM_OPEN%, so pairing worked only when the timing was lucky. B after STREAM_OPEN alone was not enough: when the base has lost its bond (U,Z, a new base) and the remote's RN4870 still holds the old one, the first connection ends in %CONNECT%%DISCONNECT% before STREAM_OPEN. The second connection always bonded (bench 2026-10-25: 0/6 first attempts before the fix, 4/4 after).

## Watch out
A pairing that times out in state 1 (SCANNING) is a different failure: the base never saw the remote advertise. Capture the remote's BLE UART (LA D6/D7) before you power-cycle the remote, or the evidence is gone. tools/basetest/test_pairing.py disconnect_and_forget (U,Z) creates the old-bond case.
