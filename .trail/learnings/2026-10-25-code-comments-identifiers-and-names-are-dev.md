---
title: "Code comments, identifiers and names are always English; only UI strings are Czech"
author: Dev
date: 2026-10-25
branch: bl-robust
cost: { minutes: 28, tokens: 96961 }
id: l-747ae030
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - CMakeLists.txt
  - STM32G431CBTx_FLASH.ld
  - bootloader/STM32G431CBTx_BOOT.ld
  - bootloader/src/main.cpp
  - cmake/git.cmake
  - cmake/merge-hex.cmake
  - inc/base.hpp
  - inc/bluetooth.hpp
  - inc/crc32.hpp
---

## Decided
The user's rule: comments, variable names and identifiers in firmware and software are always English, even though the UI is Czech. Czech text stays only in user-visible string literals (OLED/menu/screen text, ASCII-stripped Czech like "Sparovat ovladac"). CLAUDE.md in midicontrol-fw and midicontroller-fw now says so. All comments were translated in commits 8e245ba (midicontrol-fw), c935c46 (midicontroller-fw), 0d4ae8c (protocol), f2af760 (bootloader). midicontrolpy-sw was already English.
Shared code lives in canonical repos (midicontrol-protocol, midicontrol-bootloader); lib/protocol and lib/bootloader in both fw repos are clones of them. Edit the canonical repo, push it, then point the lib/ clone at the pushed commit and bump the pointer, rather than committing separately inside the clones.

## Tried and ruled out
Finding Czech comments with a diacritics grep: much of midicontroller-fw is Czech written without diacritics ("Bitove (bez tabulky)"), so every file has to be read. Renaming Czech identifiers: none were found, so nothing to do.

## Watch out
Czech in assert/print/FATAL_ERROR messages (tester.py, tools/basetest/la.py, test_display.py, merge-hex.cmake, ringbuffer.hpp static_assert) and in readme.md was left as-is; the rule may extend to non-UI messages. midi.cpp:83 comment says "upper nibble = cable number" but the code masks & 0x0F (pre-existing mismatch). protocol repo files use CRLF; keep it when scripting edits.
