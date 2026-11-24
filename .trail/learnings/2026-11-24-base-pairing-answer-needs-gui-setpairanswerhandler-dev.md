---
title: "Base pairing answer needs GUI::setPairAnswerHandler; basetest pairing must clear _reconnectNeeded"
author: Dev
date: 2026-11-24
branch: main
cost: { minutes: 1, tokens: 1882 }
id: l-dc1c0e8f
scope: repo
status: active
src: agent
touches:
  - src/main.cpp
  - tools/basetest/conftest.py
---

## Decided
- src/main.cpp must call GUI::setPairAnswerHandler(pairAnswer). Without it the base's Ano/Ne on the D3 code is lost silently: LTO folds the confirm screen into a bare nav.pop(), with no build error.
- tools/basetest/conftest.py forget_remote writes 0 to Bluetooth::_reconnectNeeded. Otherwise the base's 5 s auto-reconnect to the old remote collides with startPairing and the pairing tests fail.
- BASETEST_UPDATER can point at midicontrol-sw/build/midicontrol when the container has system rtmidi. The RtMidi shim build is only needed without it.

## Watch out
- Remote OTA tests (test_ota remote, test_boot_remote, test_web_remote_update) can fail inside a long mixed batch ("720/78648", FW_UPDATE_END rejected) and pass when run on their own. Rerun them separately before you suspect the firmware.
