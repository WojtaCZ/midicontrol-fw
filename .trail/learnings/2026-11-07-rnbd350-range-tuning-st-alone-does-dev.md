---
title: "RNBD350 range tuning: ST alone does not set the link parameters; use T and CSPHY per connection; module speaks command set v1.0"
author: Dev
date: 2026-11-07
branch: main
cost: { minutes: 2, tokens: 27446 }
id: l-343c1345
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - CMakeLists.txt
  - TODO.md
  - inc/bluetooth.hpp
  - lib/protocol
  - src/bluetooth.cpp
  - src/logic/link_stats.cpp
  - src/logic/link_stats.hpp
  - src/main.cpp
  - src/router.cpp
---

## Decided
2026-11-07 bench (DS50003684B): SGA/SGC,0F and ST,0010,0010,0000,00FA go into the config job of both nodes, but the base also sends per connection, after %SECURED% on an accepted peer, `CSPHY,<handle>,04,04,02` (Coded S8) and `T,0010,0010,0000,00FA` (src/bluetooth.cpp requestCodedPhy; remote: Bluetooth::requestCodedPhy). Supervision timeout 2.5 s (module default 720 ms), interval stays 20 ms, latency 0 (it would delay notes). The link ends Coded / 2500 ms, verified in INFO_RESP (tools/basetest/test_link_stats.py).
## Tried and ruled out
ST alone: the link made by `C` still ran 720 ms because the remote module re-proposes its own defaults; even with ST on both sides a 1-2 s flip-flop (2M / 720 ms) follows each stream open until the base's second request wins. `GICP`/`QRSS`/`SICP` (firmware 1.2 names) fail with Err: this module runs command set v1.0 (GT, M, ST). SGA/SGC have no getter, so they cannot be read back; MTP,<handle>,<phy> shows 12 dBm current / 14 dBm max.
## Watch out
`M` answers signed hex ("-2E" = -46 dBm) and needs a `$$$` session, so it is polled only when idle. %CONN_PARAM% fields are HEX (the old sscanf %d read 0048 as 48). With the 2.5 s timeout a vanished peer is detected after ~2.5-3 s, not 1 s (test_link_loss.py window is 8 s). Range itself is unverified: only a walk through the church proves it.
