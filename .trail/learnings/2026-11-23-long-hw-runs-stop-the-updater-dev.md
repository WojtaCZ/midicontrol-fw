---
title: "Long hw runs: stop the updater too, its 02:00 window recreates and starts a stopped midicontrol daemon"
author: Dev
date: 2026-11-23
branch: main
cost: { minutes: 1, tokens: 1694 }
id: l-aa3e797f
scope: repo
status: active
src: agent
touches:
  - tools/basetest/conftest.py
---

## Watch out
`docker stop midicontrol` is not enough for an overnight `tools/basetest/conftest.py` `pytest -m hw` run. The updater container
(midicontrol-sw docker/updater/lib.sh, restart=always) has an update window at 02:00 local. When ghcr has a newer `:edge` image it
recreates the app container and starts it even when it was stopped (by design: docker/tests/test_updater.sh
update_stopped_container_proceeds). The daemon then opens the base CDC (/dev/ttyACM0) and every later test fails with
"[Errno 16] Device or resource busy": on 2026-11-23 this turned 93 of 144 tests into ERRORs.

## Decided
For long hw runs: `docker stop midicontrol updater`, then `docker start midicontrol updater` afterwards, because stopping only the
app leaves the updater free to restart it at 02:00.
