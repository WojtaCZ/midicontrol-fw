---
title: "Bootloader SH1106 drawing: column start 2 and no full redraw per chunk"
author: Dev
date: 2026-10-25
branch: fw-progress
cost: { minutes: 58, tokens: 306635 }
id: l-dbe42bb7
scope: repo
status: active
src: agent
touches:
  - bootloader/src/main.cpp
  - docs/superpowers/plans/2026-10-25-update-progress-bars.md
  - lib/bootloader
  - lib/ui
  - CLAUDE.md
  - CMakeLists.txt
  - inc/menu.hpp
  - src/menu.cpp
  - src/router.cpp
  - bootloader/CMakeLists.txt
---

## Decided
- Bootloader screens (lib/bootloader src/oled.cpp, boot::oled) write SH1106 pages from column 2, 128 bytes: the apps draw with theme.displayOffsetX = 2 (130 cols/page), so the app's progress frame sits in RAM cols 6..125; column 0 put the bootloader picture 2 px left (bench I2C capture, 2026-10-25).
- boot::oled::show() skips when title/hint pointers + percent are unchanged and re-renders only the bar rows (24..39, static_assert against ui::kProgressBarY/H) on a percent change, sending only changed column ranges.

## Tried and ruled out
- Full ui::drawProgress render + dirty-page flush on every progress call: ~55 ms per call on the 16 MHz cores (per-pixel virtual setPixel). Base USB update took ~34 s (91 % CPU in redraw), remote install copy 13.1 s. After the fix: base transfer 3.5 s, remote copy 3.2 s.

## Watch out
- title/hint are compared as pointers (pass string literals); a reused buffer with new text will not redraw.
- The bar-only path depends on ui::drawProgress layout; change lib/ui/inc/ui/progress.hpp constants and the static_assert together.
