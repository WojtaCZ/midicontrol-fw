---
title: inc/git.hpp format is parsed by basetest; change both together
author: Dev
date: 2026-11-14
branch: audit/step1-docs
cost: { minutes: 1, tokens: 1650 }
id: l-52a5ef39
scope: repo
status: active
src: agent
touches:
  - cmake/git.cmake
  - tools/basetest/test_ota.py
  - inc/git.hpp
  - tools/screens/stubs/git.hpp
---

## Decided
2026-11-14 (audit step 1, BD-1): cmake/git.cmake emits `inline constexpr char x[]` instead of `static constexpr std::string` (constexpr std::string broke on branch names > 15 chars, SSO limit). Consumers use const char* / printf %s.

## Watch out
- tools/basetest/test_ota.py `git_revision()` regex-parses inc/git.hpp (`revision(?:\[\])? = "..."`); test_web_update.py imports it. Change the generated format and the regex together, or the bench OTA tests fail.
- tools/screens/stubs/git.hpp (host stub) must use the same form as the generated header.
- Code that calls `.c_str()` on git:: values (lib/menu has such code, not compiled in the fw) will not build against the char-array form.
- Never commit inc/git.hpp; every build rewrites it.
