---
title: "basetest: a new process's first GET_INFO (pid 1) is dropped as a duplicate, and un-ACKed INFO_RESP leaves stray bytes"
author: Dev
date: 2026-11-07
branch: main
cost: { minutes: 58, tokens: 209060 }
id: l-ffd2e289
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - TODO.md
  - src/bluetooth.cpp
  - tools/basetest/conftest.py
  - tools/basetest/test_link_stats.py
  - lib/protocol
  - src/logic/link_stats.cpp
  - src/logic/link_stats.hpp
  - src/router.cpp
  - test/host/test_link_stats.cpp
---

## Decided
tools/basetest/test_link_stats.py info() ACKs every INFO_RESP (Packet ACK with payload [pkt.pid], as test_cdc_local.py does) and retries GET_INFO up to 3 times with a fresh packet id. The `linked` fixture waits for the RSSI of both nodes (base polls every 7 s, remote every 11 s) before asserting.
## Tried and ruled out
Blaming the new INFO layout or the RSSI poll: 12/12 GET_INFO round trips were clean, the poll gave 10/10 good `M` replies and no `$$$ timeout` on a healthy link. A CDC TX overflow (`router::_cdcOverflows` was 0) and stuck retry slots were also not it.
## Watch out
Every Python process (usblink.Link, each pytest session, ad-hoc scripts) numbers its packets from 1, and the base drops a reliable request whose id equals the last one it executed (router duplicate detection): it ACKs but sends no INFO_RESP ("no INFO_RESP from 2"). A request that is not ACKed makes the node retransmit INFO_RESP three times; the copies reach the next process, whose parser sees a frame tail without its head and `link` teardown fails with "bytes outside frames on CDC". Do not run un-ACKing scripts between pytest runs, and give a clean base reset or a retry before blaming firmware. The INFO payload is limited to 128 bytes (the remote's is 122): new tags go last or older ones such as VBUS_MV are dropped silently.
