---
title: "OLED UI: every list is the 2-row big-font ui::drawList; draw only through the navigator (screen x=0 is RAM col 2)"
author: Dev
date: 2026-10-25
branch: main
cost: { minutes: 12, tokens: 201735 }
id: l-ef35a7fb
scope: repo
status: active
src: agent
touches:
  - tools/screens/main.cpp
---

## Decided
- The user chose (2026-10-25) the 2-row Font_11x18 list with dot/checkbox/back icons for EVERY list, including the remote's song list (was a custom 4-row 7x10 list with an inverted bar). Shared drawing lives in lib/ui `ui/list.hpp` (drawTitleBar, drawList/listTop, drawHint/hintY); app screens with their own lists must use it. Scroll arrows mean "rows hidden in that direction" (up on the first row, down at the bottom of the last), not "selection can move".
- All screens: text at theme.leftMargin = 2, title bar with a separator line at y=11, key hints / splash comments on the bottom line (y=54). Status icons (remote battery+link, base PC+BLE) are deliberately different per device.
- Progress screen layout (hint at y=48) left alone: it is shared with both bootloaders' bar-only redraw.

## Watch out
- Anything drawn straight into Oled frameBuffer (status icons, overlays) bypasses Navigator's OffsetFrameBuffer (displayOffsetX = 2): raw x=0..1 are invisible. The base's old "Sparuj ovladac" overlay was clipped that way; it is now MenuScreen::setTitle().
- Check UI changes with tools/screens/export.sh in either fw repo (host build of the real menu.cpp, PNG + sheet.png).
