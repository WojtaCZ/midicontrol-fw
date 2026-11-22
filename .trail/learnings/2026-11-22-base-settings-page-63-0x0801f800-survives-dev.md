---
title: Base settings page 63 (0x0801F800) survives every firmware update; only a full chip erase resets it
author: Dev
date: 2026-11-22
branch: main
cost: { minutes: 4, tokens: 92918 }
id: l-8cdd1e9d
scope: repo
status: active
src: agent
touches:
  - TODO.md
  - src/bluetooth.cpp
  - src/logic/ble_status.hpp
  - test/host/test_ble_status.cpp
  - tools/basetest/dbg.py
  - tools/basetest/test_pairing.py
  - src/settings.cpp
  - bootloader/CMakeLists.txt
---

## Decided
- The base settings record lives at 0x0801F800 (page 63, src/settings.cpp). The bootloader apply erases [BOOT_APP_BASE, BOOT_APP_END) = [0x08008000, 0x0801F800) (bootloader/CMakeLists.txt, lib/bootloader spiflash_apply.cpp), and the full/update hex images end exactly at 0x0801F800, so neither an OTA nor `mcu flash`/`flashApp` touches the settings.
## Tried and ruled out
- "LED setting lost on reboot" (owner 2026-11-22): save + reset round trip of ledMasterEnable 1/0 works on the bench, FLASH SR stays 0, init runs before OLED/GUI/LED. Not a flash-erase or init-order bug; needs an owner repro.
## Watch out
- The release base ELF inlines settings setters (settings::setLedMasterEnable is missing): write settings::ramCopy (+5 LED, +6 midiRole) over gdb and call settings::save() instead.
- Keep BOOT_APP_END == settings page if the linker FLASH length (94K from 0x08008000) ever changes.
