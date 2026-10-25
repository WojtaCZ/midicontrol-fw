---
title: Remote BLE name is matched exactly by the base; info screens fit ~18 characters per line
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 1, tokens: 5459 }
id: l-004187a2
scope: repo
status: active
replaces: l-ccedf111
src: agent
touches:
  - inc/bluetooth.hpp
  - src/bluetooth.cpp
  - openocd.cfg
  - tools/basetest/dbg.py
---

## Decided
- The names stay MIDIControlBase and MIDIControlRemote (user, 2026-10-25: a rename to "MIDIController" was reverted the same day). Base pairing compares the advertisement name exactly against Bluetooth::REMOTE_NAME (inc/bluetooth.hpp, main.cpp SCANNING), and the remote writes SN on every boot (midicontroller-fw src/bluetooth.cpp CONFIG_COMMANDS), so a rename must change both firmwares and be flashed together. An already bonded remote still reconnects (the base reconnects by stored address, not name).
- Bluetooth info screens (base and remote Nastaveni -> Bluetooth) show the bare name on line 0, no "Jmeno:" label.

## Tried and ruled out
- "Jmeno: MIDIControlBase": ui::ParagraphScreen does not scroll and cuts the line at ~18 characters of the small font.
- Names with a space ("MIDIControl Remote"): the RN4870 answers Err to SN and the rest of the configuration is skipped (REVIEW.md).

## Watch out
- Two ST-Link/V2 probes are attached; openocd.cfg selects none, so `mcu flashApp` may hit the wrong board. Use `adapter serial` from tools/basetest/dbg.py CONFIGS (base 39FF71064B43353943122243, remote 37FF70064E59313840161343).
