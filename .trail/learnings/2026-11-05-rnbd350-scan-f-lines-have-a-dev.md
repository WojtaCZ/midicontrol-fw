---
title: "RNBD350 scan (F) lines have a 2-digit address type, signed RSSI and two extra fields; the RN4870 parser rejected all of them"
author: Dev
date: 2026-11-05
branch: rnbd350
cost: { minutes: 26, tokens: 94556 }
id: l-d610965d
scope: repo
status: active
src: agent
touches:
  - src/bluetooth.cpp
---

## Decided
2026-11-05 bench: base pairing ended "nenalezen" with the RNBD350 because every scan line parsed as UNKNOWN (log "status type 36").
Real RNBD350 lines (base log, debug level):
  connectable      40843204625F,00,MIDIControlRemote,FEDA,-35,01,00   (scan response repeats it with empty UUID)
  non-connectable  8C79F5D3DE29,00,-59,01,00,Brcst:1BFF7500...
RN4870 was <addr>,<0|1>,<name>,<uuids>,<rssi F5> and <addr>,<0|1>,<rssi>,Brcst:<data>.
Parsing moved to blestatus::parseScan (src/logic/ble_status, host tests with both formats); bluetooth.cpp only maps it to Adv*Command.
Unrecognised status text is now logged raw at debug level ("ble: status ?: ...") so the next format surprise is visible with `level d` on the log CDC.
## Watch out
- RNBD350 RSSI "-35" is parsed as hex (like every RN number); unverified, nothing decides on RSSI.
- After a pairing timeout in SCANNING the base logged "$$$ timeout" twice while scan lines kept arriving: the scan may not be stopped on that path. Not investigated.
- Base can be updated over USB without the ST-Link: midicontrolpy-sw built in a scratch dir, --config with existing PlayFolder/RecordFolder (missing folders are fatal and the tool prints nothing else), --cmd "update base build/midicontrol-fw-update.bin".
