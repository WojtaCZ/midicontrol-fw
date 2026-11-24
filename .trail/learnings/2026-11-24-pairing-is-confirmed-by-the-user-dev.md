---
title: "Pairing is confirmed by the user on both devices (handshake), not by the module (SA,2 has no accept command)"
author: Dev
date: 2026-11-24
branch: backlog/pair-1124
cost: { minutes: 26, tokens: 120540 }
id: l-e89b4f28
scope: repo
status: active
src: agent
touches:
  - CMakeLists.txt
  - inc/bluetooth.hpp
  - inc/menu.hpp
  - lib/protocol
  - src/bluetooth.cpp
  - src/logic/peer_guard.cpp
  - src/logic/peer_guard.hpp
  - src/main.cpp
  - src/menu.cpp
  - test/host/test_pair_session.cpp
---

## Decided
D3 (owner 2026-11-24: no random phone may pair): option B, firmware-level confirmation on top of SA,2 (Just Works, the module has no accept/reject command, R34 removed the remote's "Sparovat?" because it fired on every reconnect). protocol/pairing.hpp: PAIR_REQ/RSP/CONFIRM 0x90-0x92, code from both nonces, 5 s reply and 30 s decision timeouts. A peer that secured in a user-started pairing is only pending (remote: peer_guard pending(), base: Bluetooth::setPairPending) and passes only PAIR_* frames; the bond is stored only after Ano on both devices. Ne/timeout/silent peer: K,1 + U,Z, nothing stored. Bonded reconnects skip it and never prompt (so R34 still holds).
## Tried and ruled out
Switching the module to a passkey mode (SA,1/SA,4) - the base SA,2 + remote SA,1 mix ends in %ERR_SEC% (bench 2026-10-19); no module change was made.
## Watch out
It proves intent and protocol, not MITM safety. U,Z after a rejection also wipes the real peer's bond (it re-bonds silently via SA,2). Both devices need the new firmware; an old peer fails safely after 5 s. PAIR frames are never forwarded to the PC.
