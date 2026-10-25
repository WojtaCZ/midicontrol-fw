---
title: Remote BLE name is matched exactly by the base; info screens fit ~18 characters per line
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 5, tokens: 65359 }
id: l-ccedf111
scope: repo
status: active
src: agent
touches:
  - CLAUDE.md
  - inc/bluetooth.hpp
  - inc/git.hpp
  - inc/led.hpp
  - src/bluetooth.cpp
  - src/led.cpp
  - src/main.cpp
  - src/menu.cpp
  - src/midi.cpp
  - tools/basetest/test_ble_security.py
---

## Decided
- 2026-10-25: the remote advertises as "MIDIController" (user's choice, was MIDIControlRemote). Base pairing compares the advertisement name exactly against Bluetooth::REMOTE_NAME (inc/bluetooth.hpp, main.cpp SCANNING), and the remote writes SN on every boot (midicontroller-fw src/bluetooth.cpp CONFIG_COMMANDS), so both firmwares must change and be flashed together. An already bonded remote still reconnects (the base reconnects by stored address, not name).
- Bluetooth info screens (base and remote Nastaveni -> Bluetooth) show the bare name on line 0, no "Jmeno:" label.

## Tried and ruled out
- "Jmeno: MIDIControlBase" / "Jmeno: MIDIController": ui::ParagraphScreen does not scroll and cuts the line at ~18 characters of the small font.

## Watch out
- Two ST-Link/V2 probes are attached; openocd.cfg selects none, so `mcu flashApp` may hit the wrong board. Use `adapter serial` from tools/basetest/dbg.py CONFIGS (base 39FF71064B43353943122243, remote 37FF70064E59313840161343).
- RN4870 rejects names with a space (REVIEW.md), keep the name one word, max 20 characters.
