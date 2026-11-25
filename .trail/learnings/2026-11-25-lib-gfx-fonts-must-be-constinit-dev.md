---
title: lib/gfx fonts must be constinit in flash; fonts in RAM overflowed the base stack at pairing
author: Dev
date: 2026-11-25
branch: main
cost: { minutes: 4, tokens: 29911 }
id: l-13db944c
scope: repo
status: active
src: agent
touches:
  - CMakeLists.txt
---

## Decided
lib/gfx fonts were in RAM (.data, ~10.6 KB on the base) because font had a virtual destructor, so the Font_* objects needed runtime init. Now: font destructor protected, non-virtual; font data static constexpr std::array; Font_* constinit const (lib/gfx inc/gfx/text.hpp, src/font_*.cpp, both repos). Needs C++23 (constexpr bitset string ctor), so the HOST_TESTS branch of CMakeLists.txt sets CMAKE_CXX_STANDARD 23. Base .data 12392 -> 1744 B.

## Watch out
The base hit STACK_OVERFLOW (fault kind 8) at pairing start with the fonts in RAM. After an ST-Link flash C_DEBUGEN stays set until a power-on reset, so the fault handler stops at BKPT instead of resetting: the base looks frozen (stuck on HLEDAM OVLADAC, encoder dead). Power-cycle after flashing before judging a hang.
