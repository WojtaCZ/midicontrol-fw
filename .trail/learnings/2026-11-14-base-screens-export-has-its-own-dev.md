---
title: Base screens export has its own Bluetooth stub; menu.cpp API changes break it silently
author: Dev
date: 2026-11-14
branch: audit/step1-docs
cost: { minutes: 16, tokens: 186727 }
id: l-4e50c22d
scope: repo
status: active
src: agent
touches:
  - tools/screens/CMakeLists.txt
  - tools/screens/host.cpp
  - tools/screens/stubs/bluetooth.hpp
  - src/menu.cpp
  - inc/bluetooth.hpp
  - tools/screens/export.sh
---

## Watch out
tools/screens builds src/menu.cpp on the host against tools/screens/stubs/bluetooth.hpp and the fake definitions in tools/screens/host.cpp, not inc/bluetooth.hpp. Nothing in mcu build or the gtest suite compiles it, so it broke unnoticed at dc6615d (restartState/configState/resetConfirmed/rxByteCount, blestatus::health) until the step-1 audit (fixed in 75d9727).
When menu.cpp starts calling a new Bluetooth:: function or a new src/logic file, add the stub declaration, a host definition, and the .cpp to tools/screens/CMakeLists.txt, then run tools/screens/export.sh.
