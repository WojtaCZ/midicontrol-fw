---
title: Never rebuild firmware while basetest hw tests are running
author: Dev
date: 2026-11-13
branch: main
cost: { minutes: 1, tokens: 2416 }
id: l-89d936e8
scope: repo
status: active
src: agent
---

## Watch out
Do not run `mcu build` (base or remote) while basetest hw tests run, because some fixtures (`swd_restore`) reflash `-update.hex` from disk while the image checks use the IMG loaded at session start. Otherwise the run mixes two builds and is invalid (happened 2026-11-13; the run had to be killed and redone). Build both boards first, flash, then start pytest.
