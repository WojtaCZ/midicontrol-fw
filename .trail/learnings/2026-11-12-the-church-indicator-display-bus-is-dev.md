---
title: "The church indicator display bus is shared multi-master: manual controllers can put any song number on it at any time"
author: Dev
date: 2026-11-12
branch: main
cost: { minutes: 25, tokens: 25427 }
id: l-c5a15e43
scope: repo
status: active
src: agent
---

## Decided
Owner, 2026-11-12: the base is NOT the only writer on the church indicator/display bus. Manual controllers sit on the same bus and can send any song number at any time, including while our song plays. Expected behaviour (docs/SYSTEM.md): mirror a foreign number on the base OLED and the remote, and never touch playback because of it. While recording, the display messages seen on the bus go into the recorded MIDI file as the same embedded display SysEx the player parses (song files carry the display messages inside the MIDI). PC service start does All Notes Off + organ off but leaves the indicator alone, because it is shared.

## Tried and ruled out
- Ignore numbers we did not send: no, the base/remote must reflect what the church sees.
- Stop our playback when someone else takes the indicator: no, the owner wants playback to continue.
- Blank the indicator on PC service start: no, another controller may own what is shown.

## Watch out
Do not write display code that assumes the last number we sent is what is shown. Read back the bus.
