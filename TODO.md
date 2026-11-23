# TODO

## DIN OUT running status (owner, 2026-11-22) - implemented, organ check open

- Done: `midi::send` sends running status (`src/logic/running_status.hpp`, host tests RunningStatus.*). It leaves out a repeated
  channel status, sends Note Off as Note On velocity 0, and sends the status again after 300 ms and after SysEx or system common.
  A chord change is about 1/3 shorter on the 31250 Bd wire. Bench: `test_midi.py` and `test_midi_timing.py` (they decode both modes).
- Open: play a few hymns on the real organ and listen for missing or hanging notes. If it misbehaves, rebuild with
  `cmake ... -DMIDI_RUNNING_STATUS=OFF` (full 3-byte messages, the old behaviour) and note the organ model here.

## DONE 2026-11-22: pairing from the base failed (`$$$` sent into the running FE scan) (43e11bb)
- Fix: `blestatus::staysInCommandMode()` keeps command mode during an `FE` scan; bench test_pairing.py 3/3 twice. Open edge cases (5.3): pairing while connected (covered by disconnect_and_forget + K,1), old stored address after an upgrade.

## LED setting lost on base reboot - CLOSED 2026-11-23 (owner item 13: "might have just been a ghost, ignore it")
- Not reproduced on the bench 2026-11-22 (settings::save and reload both ways, 8922d96); no code change. Note: flash page 63 (0x0801F800) survives updates but a mass erase wipes it (defaults come back, LED on).

## Stuck base / stuck notes (watchdog) - done 2026-11-07, one organ check left

- Done: after an IWDG/WWDG reset or a fault reset the base sends All Sound Off + All Notes Off on all 16 DIN channels at boot
  (`logic/recovery`, `midi::sendStuckNoteCleanup`); PA4 (SSR) is low after any reset (static GPIO init), so the organ's
  current source is off. Hardware tests: `test_fault.py` (loop stall + IWDG, "All Notes Off sent" in the log).
- Open: check on a real organ that the notes really stop (the DIN link was never verified on the bench; SYSTEM.md section 6
  gap 28 / D31).
- Closed: the PA4 pull-down question is answered by D28 (docs/HARDWARE.md section 7 item 5: no pull-down, analysis says the
  PC817 stays off while the pin floats).

## Intermittent: remote OTA staging_abort loses the base CDC (seen 2026-11-21 and 2026-11-22)
`tools/basetest` `test_boot_remote.py::test_remote_staging_abort[stop_50]` (and once `base_reset_50`) sometimes fails in a
full `-m hw` run: fwclient reports "END NAK status None", then usblink raises SerialException ("device reports readiness to
read but returned no data") because the base USB CDC disappears, and the run hangs. Alone it passes; paired with
`test_base_glitch_erase_trailer_page` it failed 1 of 2. It failed before the phase C/D merges too (bench run 5.1,
2026-11-21), so it is not a regression. Not a field issue so far (host stop at 50 % is a test-only path), but the base
dropping off USB is worth a root cause: next step is a run with base fault/reset-cause readout (`fault::` / RCC_CSR) right
after the CDC disappears.

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

## Owner answers 2026-11-16: implementation backlog (base firmware) - finished items
Source: audit/OWNER-ANSWERS.md, plans/2026-11-16-owner-answers-impl.md, plans/2026-11-23-owner-answers.md (2026-11-23 overrides older plans). All merged into main.
- C1 DONE 2026-11-22: All Notes Off at every boot (fea1ac9). Bench: DIN capture of 32 messages after a power-on boot.
- C2 DONE 2026-11-22: master LED off darkens every rear LED, no start-up colour flash (76c9820). Bench: rear LEDs with master off.
- C3 DONE 2026-11-22/23: Nahraj recording screen (894a8b0, d295364); redone 2026-11-23 like Prehravani with the same Stop icon, owner item 7 (38908a1). Bench: look on the OLED, Nahraj against the real PC.
- C4 DONE 2026-11-22: knob on NowPlaying selects Stop / Pridat sloku (2031ae9). Bench: add verse sends REPLAY_ANCHOR 0x14.
- C5 DONE 2026-11-22: repeated signal PacketId skipped (282b199).
- C6 DONE 2026-11-22: dead code removed (8ce1ec8); signalError/frontStrip kept, owner item 8 2026-11-23: front LEDs may stay unused for now, fault handling is item 8 below.
- C7 DONE 2026-11-22: Ident. MIDI under Nastaveni > Servis (46e4193).
- C8 DONE 2026-11-22: power-cut-safe settings storage, CRC slots in page 63 (626b5e8). Bench: save LED toggle, reset, value kept; pair, reset, remote kept.
- C9 DONE in code 2026-11-23: 3 ECC repairs, then the bootloader stays in update mode (lib/bootloader ac1db5d, 4c584e7). Bench: only that a normal boot still reaches the app (TAMP BKP3R reads 0xB0CE0000 after 10 s).
- C10 DONE in code 2026-11-23: bootloader bump ac1db5d, protocol e63f2f5, BP-12 screen "Chybi firmware / Pripojte PC", diag 19 STAGE_CLEAR_FAILED (4c584e7). Bench: flash only the bootloader (`mcu flashBootloader`), check the screen, then update over USB.
- C11 DONE 2026-11-22: short song codes right-aligned with blanks (47ef1fc).
- C12 DONE 2026-11-23: organ off while playing/recording = 3 s hold screen like the remote power-off, owner item 12 (708c440, 88b6826).
- C13 DONE 2026-11-22/23: base silences the organ on PC keepalive loss (SYS-PC-03, 6 s; e212c5c); remote EMERGENCY_OFF 0x15 handler (b8e04c4). Bench: stop the PC process during playback (DIN silent, SSR off within 6 s); hold MF 3 s on the remote.
- C18 DONE 2026-11-23: long press of the base Enter leaves the song list, Zpet row stays (88b6826). Bench: hold the button in the song list.
- E3 DONE 2026-11-22: per-screen timing flows (e212c5c).
- Item 8 fault audit DONE 2026-11-23: fresh-stack capture, NMI + STACK_OVERFLOW kinds, INFO FAULT_LAST (0d1831d; remote d0efa06). Bench: force stack overflow and NMI on base and remote, read FAULT_LAST from the PC, provoke a remote HardFault and find the DIAG record after reboot.
- Item 9 unused clocks DONE 2026-11-23: TIM1/TIM2/TIM4/TIM7/RTCAPB off (810035a). Bench: Servis > update via bootloader still enters the bootloader.
- D4 base half DONE 2026-11-23: status icons on the PC-lost overlay (876fe95). Open: base shows the BLE icon only when connected and static full, the remote shows wireless_1 while linked without keepalive (icon consistency, owner item 16 asked to double-check).
- Item 14 DONE 2026-11-23: origin/display-refactoring archived as tag archive/display-refactoring, branch deleted (00b0e5d).
- Base side of A1..A9 DONE 2026-11-16..21: A1 b7d13af+51d8a83, A2 b6bd127, A3 37b68e9, A4 84fd5bd, A5 aa16fd7, A6 9750846, A7 894a8b0, A8 9febe7c, A9 e81907e.

## Open bench list (base, owner present)
- Flash set: base, bootloader and remote together, then run tools/basetest.
- BP-1: force a W25Q header write failure and check the diag record and that the app still boots (needs a safe method, ask the owner).
- Q2-17: remote connected while the base updates over USB: remote shows "Zakladna: aktualizace", Play refused BUSY (A8).
- Rear LED labels and order (USB, DISPLAY, CURRENT, MIDI A, MIDI B, BLUETOOTH, pixel order 0..5) against the printed labels, colours with master LED on/off.
- Q3-11: capture the RNBD350 module UART after a connect (stuck command mode, see the remote's open item "stuck cmd mode"), and a long old-set batch.
- DIN probe (postponed since 2026-11-13): U14.17, DIN pin 4 voltage.
- Open consistency: owner 2026-11-23 item 17 (shared error / connection-lost screen, Timeout = lost connection?): proposal in plans/2026-11-23-owner-answers.md, no code until the owner approves.
