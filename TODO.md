# TODO

## DIN OUT running status (owner, 2026-11-22) - implemented, organ check open

- Done: `midi::send` sends running status (`src/logic/running_status.hpp`, host tests RunningStatus.*). It leaves out a repeated
  channel status, sends Note Off as Note On velocity 0, and sends the status again after 300 ms and after SysEx or system common.
  A chord change is about 1/3 shorter on the 31250 Bd wire. Bench: `test_midi.py` and `test_midi_timing.py` (they decode both modes).
- Open: play a few hymns on the real organ and listen for missing or hanging notes. If it misbehaves, rebuild with
  `cmake ... -DMIDI_RUNNING_STATUS=OFF` (full 3-byte messages, the old behaviour) and note the organ model here.

## DONE 2026-11-22: pairing from the base failed (`$$$` sent into the running FE scan)
- Cause: regression from 6a68acc. The engine kept command mode only after a plain `F` (`strcmp(lastCmd, "F")`), but the
  pairing job sends `FE,...`, so it sent `---` into the running scan and the module ignored every later `$$$`.
  Fix: `blestatus::staysInCommandMode()` / `isScanCommand()` (src/logic/ble_status.hpp, host tests in test_ble_status.cpp).
- Bench: test_pairing.py 3/3 twice (one earlier run ended PAIR_FAILED once, not reproduced). The link is alive ~6 s after
  PAIR_DONE (the drop + reconnect on a new bond, see the Coded PHY entry). test_pairing.py no longer uses the logic analyser
  (owner 2026-11-22: rely on what the STM32s read).
- Still open (5.3 edge cases): pairing while connected (covered by disconnect_and_forget + K,1), old stored address after an
  upgrade; scan timeout stops cleanly is covered by test_pairing_timeout_no_remote.

## LED setting lost on base reboot (owner, 2026-11-22) - not reproduced, needs owner repro

- Owner: the base menu "LED" checkbox (settings item 0, `ledMasterCallback` in `src/menu.cpp`) does not survive a base reboot.
- Checked 2026-11-22 on the bench (release base ELF, probe): writing ledMasterEnable 1 and 0 into `settings::ramCopy` and
  calling `settings::save()`, then resetting, reloads the value both times; FLASH SR stays 0; the flash page and RAM agree
  (LED=0, midiRole=0). `settings::init()` (main.cpp) runs before the OLED/GUI/LED init, and the menu syncs the checkbox from it.
- Flash page 63 (0x0801F800) is never erased by an update: the bootloader apply erases [0x08008000, 0x0801F800) and the
  full/update hex images end exactly at 0x0801F800. A full chip erase (mass erase, STM32CubeProgrammer "erase all") does
  wipe it, and then the defaults come back (LED on).
- Open: the owner's exact steps (menu toggle -> power cycle? after an OTA? after `mcu flash` with a mass erase?). Without
  a repro there is no code change.

## Stuck base / stuck notes (watchdog) - done 2026-11-07, one check left

- Done: after an IWDG/WWDG reset or a fault reset the base sends All Sound Off + All Notes Off on all 16 DIN channels at boot
  (`logic/recovery`, `midi::sendStuckNoteCleanup`); PA4 (SSR) is low after any reset (static GPIO init), so the organ's
  current source is off. Hardware tests: `test_fault.py` (loop stall + IWDG, "All Notes Off sent" in the log).
- Open: check on a real organ that the notes really stop (the DIN link was never verified on the bench; SYSTEM.md section 6
  gap 28 / D31).
- Closed: the PA4 pull-down question is answered by D28 (docs/HARDWARE.md section 7 item 5: no pull-down, analysis says the
  PC817 stays off while the pin floats).

## RNBD350 migration: checks still to run on a full bench (open since 2026-11-05)

Verified by hand on 2026-11-05: pairing (F, X, C, bond, settings saved), reconnect after a base reset with the stored
bond, `diag`/`status` round trip through the remote. NOT run, because the bench lacked the base ST-Link
(AP access fails, only IDCODE reads) and the fx2lafw logic analyzer:

- `tools/basetest` `pytest -m hw`: ran 2026-11-07 (96 passed, 21 failed). Failures are not BLE regressions: no logic analyzer attached (test_display, test_relay, test_pairing/test_ble_security UART parts; since 2026-11-22 those tests read MCU state instead), remote DWARF ELF missing (`BASETEST_REMOTE_ELF`, KeyError `&GUI::mainScreen`, test_relay), `BASETEST_UPDATE_*` unset (test_ota), DIN dead (test_midi; gap 28), `test_log::test_positions_survive_uint32_wrap` gets a stray partial line `ype 0x01 len 1` in the `level d` reply (not analysed). Re-run with the -g remote ELF. Passing now: test_ble_down (window widened to 95 s), test_link_loss (new), test_fault, test_lifecycle, test_cdc_robust.
- `test_pairing.py` and `test_ble_security.py`: the remote address is already the RNBD350 remote `40843204625F` (done;
  the old RN4870 address `D88039FE5996` remains only as the RN4870 scan-format example in `src/logic/ble_status.hpp` and its
  host tests). They assert the firmware's own `ble: cmd` log lines (DEBUG), not the UART. The firmware log text
  "RN4870 init failed" is matched by `test_ble_down.py`; renaming it is an owner question (audit Q-BD-4), leave both alone.
- `midicontroller-fw/tools/bletest` (remote over BLE): `test_inbound`, `test_robust`, `test_outbound`,
  `test_lifecycle`, `test_ota`, `SOAK_SECONDS=600 test_soak`; needs the PC BLE adapter (the remote is now bonded to the
  base: forget the bond or pair the PC first) and the logic analyzer.
- Pairing edge cases: timeout in SCANNING (no remote nearby) must stop the scan (`X`) cleanly and not log
  `$$$ timeout`; pairing a remote that still holds an old bond (retry path); the old stored RN4870 address in settings
  after an upgrade; pairing while connected (K,1 first).
- Reconnect soak: 20+ base resets and remote power cycles, link back each time without the 5 s Z/C loop.
- Base ST-Link AP access (`examination failed`): find out whether it is the probe, the SWD cable or a locked/low-power
  MCU state; until then the base is updated over USB (`midicontrol --cmd "update base ..."`).
- Boot banner shows a stale `rev` because the tracked `inc/git.hpp` is regenerated by the build: rebuild after the
  merge and flash, check `rev` matches `git rev-parse --short HEAD` (owner question Q-BD-1: untrack `inc/git.hpp`).
  The generator now emits `inline constexpr char name[]`, so branch names longer than 15 chars build again (BD-1).
- Replace the temporary `exit cmd: END/timeout` debug line in `src/bluetooth.cpp` if it is too chatty at debug level
  (it logs once per command-mode session).

## BLE range / link tuning: still to verify (2026-11-07)

Applied and verified on the bench: `SGA/SGC,0F` (module caps at +14 dBm), `ST` + per-connection `T` (20 ms / 0 / 2.5 s), Coded PHY S8 via `CSPHY` after `%SECURED%`, RSSI by `M` (hex), INFO_RESP tags 0x14 / 0x15 / 0x17 on both nodes (packed, written last: the remote INFO payload is 122 of 128 bytes), RSSI in KEEPALIVE/KEEPALIVE_ACK (`tools/basetest/test_link_stats.py`, `test_link_loss.py`).

- **Range itself is unverified**: the bench only shows the settings were applied and the RSSI plausible (-45 dBm at the bench). Walk the church with the remote and compare the Bluetooth page (signal, link losses, last reconnect) with the previous settings; if Coded S8 is a problem, S2 is `CSPHY,<h>,04,04,01`, 2M/1M is `03`.
- Coded S8 and the firmware relay (OTA): a remote update from the PC (`midicontrol --cmd "update remote ..."`, 68.8 kB) took 2 min 32 s on 2026-11-07 (3 transient NAK BUSY, retried, image accepted, remote back with the link up). The RN4870 bench notes in `../REVIEW.md` recorded 35-75 s for similar images, so this is 2-4x slower, but the RNBD350 swap and Coded S8 are not separated: an A/B run with `CSPHY` off on both nodes would tell. The relay never runs the RSSI poll (traffic is not quiet).
- A 1-2 s flip-flop (2M / 720 ms) after every stream open before the final Coded / 2.5 s state: the remote module re-proposes its defaults even with `ST` set in its config. Harmless, but see whether the remote can be silenced.
- Charting of the RSSI (PC) is not built: the values are in `/api/status` fields (`link_rssi`, `link_peer_rssi`, ...) and on the Bluetooth page only.
- Base side has no diag ring: an unexpected module `%REBOOT%` is a WARNING log line plus the counter (the remote writes diag records `BLE_MODULE_REBOOT` / `BLE_ERR_CONN`).
- The base's first connection attempt after every boot ends in `%ERR_CONN%` and a retry ~0.2 s later (older behaviour); it now counts as one link loss in `link_losses` / `link_err_conn`, so those counters start at 1 after each boot.
- Basetest conftest turns the RSSI polls off (`Bluetooth::_rssiPollDisabled`, remote `Bluetooth::rssiPollDisabled`); a base reset clears the flag, `test_link_stats.py` turns them on itself. Packet ids restart at 1 in every process and the base drops a request with the id of the last one it executed: a second script/pytest run whose first reliable request is pid 1 gets only an ACK (`test_link_stats.info()` retries).
- `test_ble_down::test_dead_rn4870_no_reset_loop_and_retry` passes alone (211 s) but failed once after other tests in the same run (timing).
- DONE 2026-11-07 (3): reconnect and pairing on the Coded PHY. The remote advertises extended, connectable, on a Coded primary + secondary PHY (`IPE,0001,0200,07,0F,03,03` + `IAE` flags/name + `A,0200,0000,01,01`; legacy and extended sets cannot both be connectable, so legacy advertising is off; `Bluetooth::extAdvMode` = 0 restores it). The base scans with `FE,01E0,0190,03,00` (1M + Coded) and connects with `CE,<type>,<addr>,05,<1M set>,<Coded set>` (reconnect and pairing; plain `C` never connects to a Coded advertiser, bench-verified). The extended set uses the same public address, the existing bond works, `B` follows `%STREAM_OPEN%` as before. Bench: fresh pairing against a remote with a stale bond 3/3, base reset and remote reset reconnect, `test_link_loss.py` + `test_link_stats.py` 8/8. Finding: a link created together with a NEW bond over Coded advertising goes silent after ~4 s (module link up, no frames; legacy advertising and later reconnects are fine), so `pairingFinished(true)` drops the link once (`K,1`) and the auto-reconnect picks the bond up ~5 s later. Cause not found. Still unverified: real range (church walk), phones/old bases cannot see the remote any more (legacy advertising off), CE job slots stay 32 B (the CE line is built into `_ceLine` at send time, token `@CE`).

## Indicator bus: check on a real display (2026-11-12)
- SYSTEM.md section 6 gaps 24/29 are CLOSED on the bench without a display (`display::connected` forced by the probe). On a real indicator bus
  check: the base's own echo never shows up as a received frame (`display::rxAccepted`/`rxRejected` stay put while only
  the base sends), a frame from another sender right after ours is mirrored, and the base never starts a frame while
  another sender is talking (LA on both directions).

## Owner answers 2026-11-16: implementation backlog (base firmware)
Source: audit/OWNER-ANSWERS.md; plan: plans/2026-11-16-owner-answers-impl.md. Work stays on branch audit/step4-manual.

Rule for every entry: user-visible change = update docs/manual (and facts) + docs/SYSTEM.md in the same commit
(CLAUDE.md); add or extend a scenario in tools/screens (flows/*.sim, tools/screens/main.cpp). Line numbers are from
2026-11-16 and may drift. VERIFY: marks anything not confirmed from code.

### C1 All Notes Off at every boot (Q2-30)
DONE 2026-11-22: cleanup runs at every boot (needsNoteCleanup removed); bench: DIN capture of 32 messages after a power-on boot, still to do (basetest test_din.py or equivalent).
- Goal: send the stuck-note cleanup after every reset, not only after IWDG/WWDG or an unannounced soft reset.
- Current code: src/logic/recovery.cpp `needsNoteCleanup(csr, unannouncedFault)` (IWDG/WWDG always; SFTRST only if unannounced). src/main.cpp:331 computes `noteCleanup`; ~:350-357 `if (noteCleanup) midi::sendStuckNoteCleanup()` after `midi::init()`. src/midi.cpp:55 sends 32 messages (CC123 even i, CC120 odd i, channel i>>1) with the blocking `send()` (watchdog kick per message, about 1 ms each).
- Change: make the cleanup unconditional in main.cpp (drop `noteCleanup` or make `needsNoteCleanup` return true). Keep `cleanupMessageCount()`/`cleanupMessage()` as they are. Keep the call after `midi::init()` and before the first loop pass. Decide whether `needsNoteCleanup` stays as a pure function (simplest: remove it and its tests).
- Depends on: none.
- Tests: update test/host/test_recovery.cpp (the Recovery.* cases for POR/pin/SFTRST now expect cleanup, or delete the function and its cases; keep PcLossGuard.* untouched). Hardware: tools/basetest, add a check that a power-on boot emits 32 messages on DIN (VERIFY: which test file covers DIN output; list with `ls tools/basetest`).
- Manual/facts: SYSTEM.md SYS-NOTE-02 (:220) text (now "every boot"); manual chapter zakladna, startup section; facts/base.md.
- Risk / open points: about 32 ms boot delay, acceptable. A cold boot with an organ on the DIN line sends notes-off before the PC is up: that is intended. VERIFY: no other caller of `needsNoteCleanup`.

### C2 Master LED off = all LEDs off (B3 / STEP4 B3)
DONE 2026-11-22: MIDI A/B gated by the master LED (logic/led_logic.hpp, LedLogic.*), static colours off and updateStatus() before the first strip update; front error strip kept; bench: rear LEDs with master off (basetest has no LED test).
- Goal: with "LED" (master) unchecked every rear LED is dark, including MIDI A and MIDI B. Also no colour flash at boot before settings are applied.
- Current code: src/led.cpp `updateStatus()` computes `master` and `roleId`; MIDI A/B use `blink ? white : (roleId ? red/green : off)` with no master gate. `rearStrip` static initialiser has MIDI A red 0.1 and MIDI B green 0.1, so a flash shows before the first `updateStatus()`. `LED::init()` (main.cpp:359) calls `update()`. frontStrip is used only by `signalError` (main.cpp ~:739-757, no callers). inc/led.hpp has a comment that MIDI colours stay lit with LED off: wrong after this change. Pixel indices: USB 0, DISPLAY 1, CURRENT 2, MIDIA 3, MIDIB 4, BLUETOOTH 5. main.cpp:228-240 ledScheduler: 50 ms.
- Change: (1) in `updateStatus()` gate the MIDI A/B pixels (and the blink-white activity) by `master`. (2) Set the static initial colours of `rearStrip` to off and call `updateStatus()` before the first `update()` in `LED::init()` (read settings first). (3) Fix the header comment. (4) Decide: `signalError` front strip stays (fault indication) even with master off. VERIFY with owner: expected yes (safety), not stated in OWNER-ANSWERS.
- Depends on: none (C8 only if settings layout changes).
- Tests: no host test for led.cpp today. Add a small pure function in src/logic (for example `midiLedColour(master, roleId, blink, isA)`) and test it in test/host (new test_led_logic.cpp, add to test/host CMake). Hardware: tools/basetest has LED tests (`ls tools/basetest | grep -i led`; VERIFY name).
- Manual/facts: SYSTEM.md indicator/LED rows (VERIFY row ids by grep "LED"); manual zakladna chapter, LED table and Nastaveni > LED; facts/base.md "LED master off but MIDI A/B LEDs lit" bug entry becomes fixed. Bench: see "Bench (base)" rear LED labels.
- Risk / open points: owner wants rear LED labels checked on the box; colours on the panel may be swapped (bench item).

### C3 Nahraj starts / stops recording (M-recording-start, Q2-22, with A7)
DONE 2026-11-22: RecordScreen (Priprava / Nahravam + Enter = Stop / Zastavuji / refusals) driven by recording::Machine; flows 26_record, 26b_record_refused; bench: Nahraj against the real PC (B11), NAK reasons shown as expected.
- Goal: the base menu entry "Nahraj" starts a recording on the PC and stops it; recording is started only from the base. One recording at a time.
- Current code: src/menu.cpp:111 `ui::MenuItem("Nahraj", recordCallback)`; :461 `recordCallback` is empty (STEP4 B1: does nothing). Protocol: `RECORD_SONG` 0x12 and `SIGNAL_RECORDING` 0x31 are reserved (lib/protocol inc/protocol/types.hpp; PROTOCOL.md lines 76, 105, 157). Router: src/router.cpp:416 already routes `SIGNAL_RECORDING` to `observeSignal` (no case there yet; it falls to `default`).
- Change: (1) After A7 defines the payload, `recordCallback` sends `RECORD_SONG` base -> PC through the same send path as `onPlaySongSelected` (main.cpp ~:340 callbacks; copy that pattern: callback registration in GUI, send in main.cpp). (2) New screen state "Nahravam" (a RecordingScreen or a mode of NowPlayingScreen; keep a state machine in src/logic like now_playing.* so it is host-testable): Enter = stop. (3) `observeSignal` handles `SIGNAL_RECORDING` (started/stopped) so the OLED follows the PC. (4) Unpaired / no PC: show the existing "Cekam na PC" overlay, do not send. (5) While playing: Nahraj refused (one at a time). Czech strings (no diacritics): "Nahravam", "Enter = Stop", "Nahravani nelze: hraje". VERIFY wording with owner.
- Depends on: A7 (protocol payload and PC semantics), B11 (PC side).
- Tests: new test/host/test_recording.cpp for the state machine; sim flow tools/screens/flows/26_record.sim (+ `event recording <start|stop>` in tools/screens/sim.cpp and a scenario in tools/screens/main.cpp); update 03_main_noop_record_unpaired.sim (name says "noop record").
- Manual/facts: manual zakladna (menu Nahraj), postupy (new "Nahrani sloky"), SYSTEM.md SYS-REC-01 (E2: base only); facts/base.md bug "Nahraj dead".
- Risk / open points: VERIFY how the PC reports "recording refused" (NAK reason); NakReason list has no "busy recording" code except BUSY 0x04. VERIFY what the Remote displays (D10).

### C4 Knob while playing: Stop / Pridat sloku (SYS-USE-24)
- Goal: on the now-playing screen the encoder Up/Down toggles between "Stop" and "Pridat sloku"; Enter runs the selected action. Add verse sends REPLAY_ANCHOR (0x14) to the PC.
- Current code: src/menu.cpp `NowPlayingScreen::handleInput` handles only Enter (stop). State machine src/logic/now_playing.* (Playing/Stopping/Stopped). Stop callback `GUI::setStopCallback(onUserStopPlayback)` registered at main.cpp ~:340. No base code sends REPLAY_ANCHOR today (grep finds it only in the protocol). SIGNAL_ANCHOR label is shown via `GUI::setNowPlayingAnchor` (router.cpp:289 `observeSignal`).
- Change: (1) add a selection state (Stop | Pridat sloku) to the now-playing logic; Up/Down toggle, default Stop. (2) New callback `GUI::setAddVerseCallback(onAddVerse)`; main.cpp builds `Packet{src=ADDR_BASE, dest=ADDR_PC, msgType=REPLAY_ANCHOR, payloadLen=0}` and sends it with ACK/retry like STOP_SONG (VERIFY: which helper main.cpp/router.cpp uses for the stop packet; copy it). (3) Show "Pridat sloku" only after the first SIGNAL_ANCHOR (as on the remote) and only if the song has sections (A6; until A6 exists, show it after the first anchor). (4) NAK NOT_PLAYING (0x05) -> ignore or short message; NAK BUSY -> retry once. (5) Newer add-verse supersedes older pending one is PC side (B5).
- Depends on: A6 (section flag), B5 (PC).
- Tests: extend test/host/test_now_playing.cpp (selection toggle, Enter per selection); new flow tools/screens/flows/26b_now_playing_addverse.sim, also fix m03_manual_pridat_sloku.sim (it documents add verse: check what it does today, VERIFY).
- Manual/facts: SYSTEM.md SYS-USE-24; manual zakladna (Prave hraje), postupy "Pridat sloku", facts/base.md open Q "add verse by encoder not implemented".
- Risk / open points: NowPlayingScreen is drawn with the `lib/ui` / `lib/menu` widgets: do NOT edit submodules; draw with what menu.cpp already uses (VERIFY it can show two selectable rows; otherwise draw text lines manually like the current screen).

### C5 observeSignal skips repeated PacketId (Q2-21b)
- Goal: a retransmitted signal packet must not re-run its OLED action (today a repeated SIGNAL_STOPPED/PLAYING wakes the OLED again for 5 s).
- Current code: src/router.cpp:289 `observeSignal`; callers :418 (signals addressed to the Base) and :501 (PC -> Remote forward path, "observe only"). Dedup exists only for ACKed packets: :355 `_dedup.isRepeat(pkt.source, pkt.packetId)` inside `handleLocal`, state `static routercore::RxDedup _dedup` at :93, class in src/logic/router_core.hpp:39. Signals are not ACKed, so they bypass the dedup. :528 `_dedup.forget(ADDR_PC)` on PC loss.
- Change: add a second small filter for signals: remember the last PacketId per source (PC only) in router_core (for example `class SignalDedup { bool isRepeat(src, id); void forget(src); }`) and call it first in `observeSignal`; the forward path still forwards the packet unchanged (the Remote does its own dedup). Reset it together with `_dedup.forget(ADDR_PC)` at :528 and on A1 session reset. Compare only the last id (a signal with the same id as the last one is a repeat); PacketId wraps 8-bit, a new PC process starts from 1 again (Q1-8 b), hence the reset.
- Depends on: A1 (session reset rule; works without it).
- Tests: extend test/host/test_router_core.cpp (repeat, new id, forget, wrap 255 -> 0).
- Manual/facts: none visible except SYSTEM.md router/dedup row (VERIFY row id: grep -n "dedup" docs/SYSTEM.md).
- Risk / open points: VERIFY whether the PC resends signals with the same id on purpose (retries) or reuses ids across different signals. Check midicontrol-sw packet id allocation (`grep -rn packetId` there) before choosing "last id only".

### C6 Remove dead firmware code (Q2-27)
- Goal: delete code with no callers listed in the audit; keep protocol items (RECORD_SONG, SIGNAL_RECORDING) reserved.
- Current code: list at audit/STEP2-REPORT.md lines 255-290 (dfuCheck and others); STEP4 facts mention `dfuCheck` has no callers; `signalError` (src/main.cpp ~:739-757, declared ~:95) has no callers; led.cpp frontStrip is only used by it.
- Change: for each entry in that list: re-grep for callers (`grep -rn <name> src inc test tools`), delete the function and declaration, drop includes that became unused, fix CMake lists if a file becomes empty. Decide on `signalError`/frontStrip: it is a fault indicator, not dead by purpose; VERIFY with owner before removing (not clearly covered by Q2-27). Do not touch lib/*.
- Depends on: C2 (the same led.cpp area) to avoid conflicts; C3 uses `SIGNAL_RECORDING`: keep it.
- Tests: full host test run (`mcu test`) and `mcu build`; `mcu size` before/after for the commit message.
- Manual/facts: only if a removed item was documented (VERIFY: grep the manual facts for each name).
- Risk / open points: check hardware tests in tools/basetest do not call a removed feature.

### C7 Move "Ident. MIDI" to a service area (Q2-29c)
- **Owner answer 2026-11-16 (follow-up):** the submenu name `Servis` is approved (resolves VERIFY (1) below).
- Goal: the Ident. MIDI checkbox leaves the normal Nastaveni list and moves to a service area (hidden from the normal user).
- Current code: src/menu.cpp `settingsItems` = LED, Ident. MIDI, Napajeni varhan, Verze FW, Displej, Bluetooth, Sparovat ovladac, Zpet (indices 0..7; `checkboxPower` is index 2; :570-571 set checked state; :499-505 callbacks). Accessors in inc/menu.hpp: `navigator()`, `checkboxPower()`, `checkboxLedMaster()`, `checkboxMidiRole()`. Settings field `midiRoleId` in src/settings.cpp / inc/settings.hpp.
- Change: (1) VERIFY what "service area" means (owner: Q2-29c in audit/OWNER-ANSWERS.md; read the exact answer before coding). Proposed: a new submenu "Servis" reached from Nastaveni (last item before Zpet) holding Ident. MIDI. (2) Re-index all `settingsItems[n]` uses (:570-571 and any in main.cpp/sim) or look up by pointer. (3) Keep the setting persisted as today. (4) Update `checkboxMidiRole()` users in tools/screens/host stubs.
- Depends on: none (touches the same list as C8 only by index).
- Tests: flows 04_settings_checkboxes.sim, 05_settings_info_pages.sim, 06_settings_back.sim (item order changes); manual flows m01..m10 re-run (`tools/screens/run-sim.sh`).
- Manual/facts: manual zakladna Nastaveni table; re-export screenshots with the command in docs/manual/README.md; SYSTEM.md settings row (VERIFY id).
- Risk / open points: owner's exact wording of "service area" must be read; do not invent a PIN or key combination.

### C8 Full P2-4 settings storage (Q2-9)
- Goal: power-cut-safe settings: flash status check, read-back verify, and a layout that survives a power cut during save.
- Current code: src/settings.cpp `save()`: erase last page (0x0801F800), program 64-bit words, clear SR, lock, log "saved"; no SR error check, no verify. Struct: magic u32 (0xBEEFCAFE), version u8 (2), ledMasterEnable, midiRoleId, _pad, remoteAddr[13]. A power cut between erase and program loses all settings (including the paired remote address).
- Change: (1) after erase and each program wait for BSY and check `FLASH->SR` error bits (PGSERR, PROGERR, WRPERR, PGAERR, SIZERR, MISERR, FASTERR, FASTERR; VERIFY the G431 bit list in the SVD); clear them, return false. (2) read back and compare; on mismatch retry once, then log ERROR and keep RAM copy. (3) layout: use two records alternating inside the page is impossible with a page erase; use two pages (the previous page 0x0801F000 if unused; VERIFY layout in CLAUDE.md "flash layout" and the linker script so the app does not overlap) with a sequence number and CRC; load = newest valid. Or rotating 32-byte slots in one page (append; erase only when full): fewer erases, power-cut safe, preferred. (4) bump `version` to 3 with a migration from v2 (copy fields). (5) `save()` returns bool; the menu shows nothing new unless it fails (VERIFY: owner does not want a new error screen).
- Depends on: none (C7 adds no field; if C7 changes the struct bump once).
- Tests: put the slot/CRC logic in src/logic/settings_store.* with a RAM flash model; new test/host/test_settings_store.cpp (power cut after each write step, newest-valid selection, full page, v2 migration, bad CRC). Hardware: tools/basetest settings persistence test (VERIFY name).
- Manual/facts: SYSTEM.md settings storage row (VERIFY id); no manual change unless the pairing note changes.
- Risk / open points: the bootloader and app share flash: check lib/bootloader layout.hpp for the last pages before choosing the layout. Not bench-verified until tested with a real power cut.

### C9 Count ECC repairs, give up to safe mode (Q2-10, BP-2)
- Goal: the ECC NMI repair must not loop forever: count repairs, check the erase result, and after N failures stop repairing and go to a safe mode.
- Current code: bootloader repo (shared by base and remote), src/ecc_guard.cpp `handleNmi()`: reads FLASH->ECCR, `decide()` picks the page, unlock, `erasePageAt(page)` (+ vector page), lock, writes ECCR back, `NVIC_SystemReset()`. Erase result is not checked; no loop guard. Added in bootloader 21dca81 (ECC NMI + trailer).
- Change: (1) keep a repair counter that survives reset: a backup register (RTC BKP / TAMP) if unused, or a RAM word in the no-init section; VERIFY what the bootloader already keeps across reset (grep `noinit|BKP|TAMP` in the bootloader and layout.hpp). (2) check the result of `erasePageAt` (return value or SR). (3) if count >= 3 (VERIFY the number with owner; propose 3): do not repair, do not boot the app, stay in the bootloader (the update mode that waits for an image) so a USB or BLE update can heal it. This is the safe mode; BP-12 (below) is related. (4) clear the counter after a boot that stays up (the app clears it with the diag boot record; needs the same address in the app: pass it through the shared layout header). (5) log in the diag ring if the app is reached.
- Depends on: C10 (bump brings the bootloader code into the base fw).
- Tests: tests/ in the bootloader (host, has test_apply.cpp pattern): add test_ecc_guard for `decide()` + counter logic with a RAM model. Work is done in the midicontrol-bootloader repo; no change to lib/bootloader inside fw (submodule only bumped, see C10).
- Manual/facts: manual sprava/problemy: "zakladna se po vypadku proudu spusti jen do aktualizace" (VERIFY wording); SYSTEM.md SYS-FW-03 row.
- Risk / open points: bench needed (cannot make an ECC error on demand without a power cut during a flash write). VERIFY safe mode behaviour on the base (waits for a PC: BP-12) and on the remote.

### C10 Bump lib/bootloader to 229d79b, BP-1 in diag, BP-12 (Q2-13)
- Goal: pin the base fw to bootloader 229d79b (BP-1 fix), show the recorded apply-clear failure in diag, and handle BP-12 (the base bootloader waits forever for a PC).
- Current code: `git submodule status lib/bootloader` = a488f5e (docs only after 19e5fdb). What changed in the bootloader since the pin:
  - Bootloader log, newest first (main repo, checked): a5d7a91 CLAUDE.md manual rule; dde171d docs sync; 19e5fdb docs sync; 229d79b "BP-1: record a failed clearStagePending() and still boot the app" (adds `stageClearFailed()` in inc/boot/spiflash_apply.hpp, `s_clearFailed` in src/spiflash_apply.cpp, tests/test_apply.cpp; apply result unchanged; not bench-verified); a488f5e (the current pin); then 16d99a4, 560809b docs; 21dca81 ECC NMI recovery and trailer required to boot (already in the pin).
  - So the code delta of the bump is 229d79b only; a488f5e is NOT an ancestor-successor of it (229d79b is one commit newer). The other commits up to 229d79b are docs. `git merge-base --is-ancestor 229d79b a488f5e` is false (checked).
  - `stageClearFailed()` has no caller anywhere (BP-1 "no caller").
- Change: (1) bump `lib/bootloader` to 229d79b (Q2-13 names that commit; `git -C lib/bootloader fetch` then checkout 229d79b, then stage the gitlink in the fw repo; do this only when the owner starts C10); build base to confirm cmake/bootloader.cmake still works. (2) BP-1 in diag: the bootloader and the app are different binaries; the failure flag is in the bootloader RAM. Either the bootloader writes it into a persistent word (same as the C9 counter area) and the app reads it at boot and adds a diag record (protocol diag.hpp `Code` enum, new code e.g. STAGE_CLEAR_FAILED = 19 and node-specific; this is a protocol change: ask the protocol repo, A-list has none: VERIFY) or the app reads the W25Q header itself. (3) BP-12: bootloader main.cpp:123 (STEP2 row BP-12) waits forever for a PC when there is no valid app; owner 2026-11-22: NO timeout (waiting is the recovery path); show a screen message while waiting, e.g. "Chybi firmware" / "Pripojte PC" (Czech without diacritics, check the width). The bootloader needs a minimal display write for that; check its flash budget.
- Depends on: none for the bump; C9 shares the persistent word; a protocol constant needs the protocol repo.
- Tests: bootloader host tests (cmake in midicontrol-bootloader, build-host); fw host tests unchanged; bench (Bench (base) list).
- Manual/facts: SYSTEM.md copy in lib/bootloader is synced from the main copy (tools/sync-system-doc.sh); manual problemy for "aktualizace selhala".
- Risk / open points: not bench-verified. Do not commit inside the submodule from this repo; bootloader changes are made in midicontrol-bootloader and then pinned.

### C11 Indicator: short song numbers right-aligned with blanks (check current code)
- Goal: a short song code shows right-aligned with blank leading digits on the church indicator and the OLED (for example "  12", "401 " style per PROTOCOL.md blank-digit rule).
- Current code: src/display.cpp:311 `digitChar(state)` returns ' ' for a nibble with the 0xF high/blank pattern; :312 `formatSong` reads currentState[6..3] (thousands..units) and :320 `formatVerse` currentState[2..1]; :211 `applyNowPlaying` applies NOW_PLAYING_INFO bytes (BCD, nibble 0xF = blank). :264 `setSong(uint16_t, bool visible)` and :278 `setVerse` build the digits for the base-local writes (VERIFY how leading zeros are handled: read lines 258-310). So alignment is decided by the sender: with NOW_PLAYING_INFO "0xFF 0x12" the PC already gives right-aligned blanks.
- Change: (1) read `setSong`/`setVerse` and `applyNowPlaying`; test with a short code (12, 401, 1999, 3). (2) if leading zeros appear, replace them with blanks (0xF) in `setSong`, keeping the last digit always shown. (3) verse "1 " per PROTOCOL.md (`0x1F`): the protocol example shows verse LEFT-aligned ("1 "): do not right-align verses; VERIFY with the indicator bus doc (midicontrol-protocol/INDICATOR-BUS.md) before changing `setVerse`. (4) STEP4 base facts say "thousands digit not capped": cap song at 1999 in `setSong` (Q1-2 rule).
- Depends on: A3 (display writer tracking).
- Tests: test/host/test_indicator_bus.cpp and test_status_framer.cpp are the nearest; add cases for the frame bytes of short numbers; sim: flows with `event playing 12` and `expect text`.
- Manual/facts: manual zakladna "Co ukazuje tabule" and tabule chapter; facts/base.md.
- Risk / open points: "check current code" in the answer means the result may be: already correct. If so, add the tests only and close.

### C12 Confirm before turning the organ off while playing or recording (Q2-29b, moved from remote D6)
- Goal: owner 2026-11-16: `Napajeni varhan` OFF on the base asks for confirmation when a song plays or a recording runs; otherwise it switches at once.
- Current code: the SSR checkbox is in Nastaveni (`src/menu.cpp`, `base::current`); `NowPlayingScreen` knows the playing state from `SIGNAL_*`; no recording state on the base yet (A7/C3). VERIFY the exact checkbox callback line before coding.
- Change: in the checkbox callback, when switching OFF and (playing || recording) push `ui::ConfirmScreen` "Vypnout varhany?" (18 chars max, 7x10 font); `Ano` -> switch off (the PC sees it via STATUS, A5 "by menu"), `Ne` -> checkbox stays on. Switching ON never asks.
- Depends on: A7/C3 for the recording state (playing half can go first).
- Tests: base sim flow `m*_organ_off_confirm.sim` (playing -> uncheck -> expect confirm; Ne keeps SSR on; stopped -> uncheck switches at once); tools/screens/main.cpp scenario for the new screen.
- Manual/facts: `zakladna.md` (Napajeni varhan), `facts/base.md`; SYSTEM.md if a SYS row covers it.
- Risk / open points: none beyond the VERIFY above.

### E3 (base) Per-screen timing checks in sim flows (Q3-9)
- Goal: sim flows assert the real per-screen timeouts, not only the screen ids.
- Current code: tools/screens/sim.cpp supports `tick <ms>` (30 ms steps, one-shot schedulers, splash auto-dismiss at 3 s, OLED sleep at 30 s idle), `expect screen|text|notext|scrolled|var`. Existing timing flows: 02_splash_autodismiss, 09_songs_timeout_empty (events `songs_delay`, `songs_timeout` fired by hand, not by time), 20_oled_sleep, 21_sleep_first_key_main. Base timers: song list delay 500 ms and timeout 3 s (menu.cpp), splash 3 s, OLED sleep 30 s, signal-driven 5 s OLED wake (C5), "Zastaveno" display after stop (VERIFY duration in now_playing.*), pairing screen timeouts (VERIFY), FW update screen over nopc (flows 23).
- Change: (1) collect the timeouts per screen from src/menu.cpp and src/logic/now_playing.* (grep `_ms\|Ms\|kTimeout`); (2) for each, add flow steps `tick <T-100>`, `expect screen <before>`, `tick 200`, `expect screen <after>`; (3) change 09 so the songs timeout fires by time (`tick 3500`) instead of `event songs_timeout` (keep one flow with the manual event); (4) new flows 26.. named `t01_timing_*.sim`; (5) add a flow-level name list to run-sim.sh if flows are enumerated (VERIFY: run-sim.sh globbing). (6) the exact timeouts also go into docs/manual/facts/base.md timing table.
- Depends on: C3/C4 add screens: write their timing steps with them.
- Tests: these are the tests; run `tools/screens/run-sim.sh`.
- Manual/facts: facts/base.md (timeouts), SYSTEM.md timing glossary (:442-454) if a value was wrong.
- Risk / open points: sim time is virtual; it does not prove the OLED driver timing. Hardware timing stays in the bench list.

### Base side of the A-items (details in midicontrol-protocol/TODO.md)
- Base side of A1: see midicontrol-protocol/TODO.md. Base touch points: src/router.cpp:93 `_dedup` (RxDedup, src/logic/router_core.hpp:39), :355 `isRepeat` in `handleLocal`, :528 `_dedup.forget(ADDR_PC)` on PC loss. Needed: reset `_dedup` (and the C5 signal filter) when the PC session nonce changes (PC hello); re-enable the 2 DISABLED PM-13 tests in test/host (grep -rn DISABLED_ test/host; VERIFY where).
- Base side of A3: see midicontrol-protocol/TODO.md. Base touch points: src/display.cpp:211 `applyNowPlaying` and :258-302 `clear/setSong/setVerse/setLetter/setLed` (the indicator writer); router.cpp:395 NOW_PLAYING_INFO case. Needed: remember who wrote the last number (PC or base menu) and clear on SIGNAL_STOPPED/stop only the own number.
- Base side of A4: see midicontrol-protocol/TODO.md. Base touch points: src/router.cpp:508 `_remoteFw.observe` (FW_UPDATE_STATUS handling for the remote) and the base's own FW_UPDATE_* handling (grep FW_UPDATE in src/router.cpp, src/usb.cpp; VERIFY file). Needed: the ACK + retry of FW_UPDATE_STATUS to the PC and a clearer error for PM-8.
- Base side of A5: see midicontrol-protocol/TODO.md. Base touch points: src/router.cpp:311-390 INFO_RESP builder (GET_INFO at :385), src/menu.cpp organ power checkbox (`checkboxPower`, menu callback sets power), src/base.cpp current::enable/disable. Needed: a source field (PC vs menu) kept in base::current and a new INFO tag (inc/protocol/info.hpp).
- Base side of A7: see midicontrol-protocol/TODO.md. Base touch points: src/menu.cpp:111, :461 (C3), src/router.cpp:416 (SIGNAL_RECORDING case), main.cpp callbacks ~:340.
- Base side of A8: see midicontrol-protocol/TODO.md. Base touch points: src/router.cpp:508 (`_remoteFw`), router.cpp forward of PLAY_SONG (refuse with NAK BUSY while the base or the remote updates), menu.cpp fwupdate screen (flows 17, 23, 24) for "<device> updating" text.
- Base side of A9: see midicontrol-protocol/TODO.md. Base touch points: src/menu.cpp `MAX_SONGS = 32`, `_songNames[32][24]`, `_songListItems[33]`, auto-load condition in `handleKey` (`_songCount < MAX_SONGS`), `handleSongListResponse` (bounded by MAX_SONGS), timers 500 ms delay and 3 s timeout; router.cpp:400 SONG_LIST_RESP; flow 12_songs_cap32.sim and 11_songs_paging.sim. Needed: a window of items (ring or page offset) instead of a fixed array; RAM budget check (`mcu size`).

### Bench (base)
Owner present. Each item: what to do, what to record.
- Flash set: flash the new base, bootloader and remote together (after C10 the bootloader pin changed); run tools/basetest afterwards.
- BP-1 (Q2-13): force a W25Q header write failure (for example hold the chip's CS or WP low during the clear step; VERIFY a safe method with the owner) and check the diag record and that the app still boots.
- Q2-17: remote connected while the base updates (FW update over USB): check the remote shows "Zakladna: aktualizace" (D11) and Play is refused BUSY (A8).
- Rear LED labels and order: compare the six rear LEDs (USB, DISPLAY, CURRENT, MIDI A, MIDI B, BLUETOOTH; pixel order 0..5) with the printed labels; check colours with master LED on and off (C2).
- Q3-11: capture the RNBD350 module UART after a connect (the "stuck command mode" problem, memory note "remote stuck cmd mode"); and run an old-set long batch (the long-run tools in tools/basetest; VERIFY which test).
- DIN probe (postponed since 2026-11-13): check U14.17, DIN pin 4 voltage (V).
- ECC repair (C9): no on-demand method; bench only the normal boot path unchanged.
- Battery items, ProgramOffset -1: not base (remote and organ/PC side).

### C13 Base-side silence when the PC stops while a song plays (owner 2026-11-22, with midicontrol-sw B13)
- Goal: second safety layer for hung notes: if the PC keepalive or MIDI stream stops during playback, the base sends All Notes Off on DIN OUT (and, if the owner agrees, switches the organ off) within the 10 s budget. Also handles the remote emergency-off message from midicontroller-fw D14 (LATER).
- Open: a playing song can legitimately send no MIDI for seconds (long held notes), so key the trigger on the PC keepalive, not on MIDI silence. VERIFY the keepalive period fits 10 s.

