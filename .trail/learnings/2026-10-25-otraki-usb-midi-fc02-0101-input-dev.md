---
title: "Otraki USB-MIDI (fc02:0101) input can wedge; replug it before blaming the base DIN OUT"
author: Dev
date: 2026-10-25
branch: ui-consistency
cost: { minutes: 7, tokens: 56574 }
id: l-62c46713
scope: repo
status: active
src: agent
touches:
  - tools/basetest/test_midi.py
---

## Decided
When tools/basetest/test_midi.py USB->DIN tests get 0 bytes but DIN->USB passes, replug the Otraki USB first.
On 2026-10-25 (after swapping the DIN in/out cables while connected) the Otraki IN delivered one corrupted message
(0x90 arrived as 0xD8), then nothing at all until a USB replug. After the replug: 4x full test_midi.py 7/7 PASS.

## Tried and ruled out
Base firmware: during the dead period USART3 had UE/TE set, PB10 was AF7 and idled high, and ISR.TC was 0 in 46% of
openocd polls while streaming (0% when idle), so the base was transmitting. The logic analyzer (la.py) has no DIN channel.

## Watch out
Swapping DIN cables hot while the Otraki is connected is a likely trigger. Check the base's TX with the TC poll before replugging.
