---
title: "Bench scripts: never take RAM addresses from an older ELF after reflashing"
author: Dev
date: 2026-10-26
branch: device-info
cost: { minutes: 30, tokens: 640321 }
id: l-ec85bba5
scope: repo
status: active
src: agent
touches:
  - cmake/git.cmake
  - CLAUDE.md
  - lib/protocol
  - src/router.cpp
---

## Watch out
A scratch helper read the SSR port pointer at 0x20005050 (from the previous ELF); after a reflash that address held
base::Encoder::storedCode, the write went to 0x00000A1A (flash alias) and latched FLASH_SR PGSERR|SIZERR|PGAERR (0xE0),
which can make the next settings save fail. Cleared by writing the bits back to FLASH_SR (0x40022010).
## Decided
Peripheral pokes use fixed register addresses (SSR = PA4: GPIOA BSRR 0x48000018); symbols always via dbg.Target.sym()
from the ELF that is actually flashed.
