---
title: "display-refactoring branch archived as tag, superseded by lib/stmcpp"
author: Dev
date: 2026-11-23
branch: main
cost: { minutes: 1, tokens: 947 }
id: l-ecf8ad49
scope: repo
status: active
src: agent
---

## Decided
2026-11-23: origin/display-refactoring (3 commits from 2024-05/06 on top of "Migrating to CMSIS", stm-cpp pin init) was never merged. main does the same job with the `lib/stmcpp` submodule (see `.gitmodules`). The branch is kept as tag `archive/display-refactoring` (c2f61ba) and deleted on origin.

## Watch out
Do not revive or merge it. If you need anything from it, cherry-pick from the tag onto the current `lib/stmcpp` code.
