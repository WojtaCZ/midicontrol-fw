<!-- COPY of midicontrolpy-sw/docs/SYSTEM.md. Do not edit here: edit the main copy, then run midicontrolpy-sw/tools/sync-system-doc.sh -->

# MIDIcontrol system specification

Version 2026-11-14. Owner decisions D1-D31 applied (binding). Items marked **(proposed default)** come from
P1-P12: technical choices the owner delegated to the developers (D30); change them only with a reason. Code reference (branch audit/step1-docs): base
`midicontrol-fw` f3302ee, remote `midicontroller-fw` 29c9a5c, bootloader `midicontrol-bootloader` 16d99a4, protocol
`midicontrol-protocol` 6fb3efb, PC `midicontrolpy-sw` c45c17a (the docker branch is not covered).
Update 2026-11-15 (audit step 2): the owner answers of 2026-11-14/15 (Q1-1 b+c, Q1-2 a, Q1-8 b, Q1-11 b, Q1-22 a,
Q-UI2-4 and the approved proposals P1-7..P1-27 listed in `audit/STEP1-REPORT.md`) are requirements. Step-2 code is on
branch `audit/step2-review` (not merged, not flashed): base b78d891, remote 43bb5fd, PC 5c42931, protocol dc4806f
plus docs, bootloader 229d79b. Behaviour implemented there without an owner decision is marked **pending (Q-id)**;
the Q-ids are listed in `audit/findings/step2/phase-d-docs.md` and in the fix reports next to it.
Wire protocol: `midicontrol-protocol/PROTOCOL.md`.

> **How to use this document**
>
> 1. The main copy lives in `midicontrolpy-sw/docs/SYSTEM.md`. The other repos hold identical copies,
>    updated by `midicontrolpy-sw/tools/sync-system-doc.sh`. Edit only the main copy, then run the script.
>    The script never writes into a submodule; `--check` also compares the copies in the firmware repos'
>    `lib/protocol` and `lib/bootloader` and reports a stale one (fix it in that repo, then bump the pointer) (P1-25).
> 2. This document states what the **owner expects** the system to do. The expected behaviour is the spec;
>    the code is measured against it.
> 3. When code changes behaviour, update the "Status vs code" column (and sec. 6 Gaps).
> 4. If the code and this spec disagree, **ask the owner**. Do not "fix" the spec to match the code.
> 5. Requirement IDs are stable. New requirements get the next free number in their group.

Status vs code values:

| Value | Meaning |
|---|---|
| implemented | Code does it; verified on the bench |
| implemented, not bench-verified | Code does it; no bench/hardware proof yet |
| partial | Some of the required behaviour is missing |
| NOT IMPLEMENTED (gap) | Required, code does not do it |
| differs from code (gap) | Code does something else than required |

Every row with `partial` or `(gap)` appears in sec. 6 Gaps.

---

## 1. Purpose and actors

The system plays recorded hymn accompaniment (Standard MIDI files) on a church pipe organ, records new songs
played by the organist, and shows the hymn number on the existing church indicator board ("tabule"). The
**organist** picks a song code (e.g. `406A`) on a battery **remote** and presses play, pause/resume, stop or
"add verse". The remote talks over BLE to the **base**, which connects to the organ (MIDI DIN, an SSR that
switches organ power) and to the church **indicator bus**. The indicator bus is **shared**: manual
controllers in the church also write song numbers to it. The **PC** (Linux mini-PC, systemd service) holds the
song files, plays and records them over the base's USB-MIDI port, decides the verse structure, owns the organ
power policy and serves a status web page.

```
                    BLE (RNBD350, Transparent UART, Coded PHY S8)
   +-----------+   <---------------------------------------->   +-----------------------------+
   |  REMOTE   |              protocol frames                   |            BASE             |
   | (0x03)    |                                                |  (0x02) STM32G431           |
   | STM32G071 |                                                |  OLED + encoder menu        |
   | OLED, keys|                                                |  USART3 31250 Bd <-> MIDI DIN <-> ORGAN
   | Li-ion 2P |                                                |  PA4 SSR driver  --> SSR ----> organ power
   +-----------+                                                |  USART1 1200 Bd <-> SHARED INDICATOR BUS
        ^ organist                                              +-----------------------------+    ^
                                                                      ^ USB composite               | manual
                                                                      | CDC0 protocol, MIDI, CDC1 log | controllers
                                                               +-----------------------------+
                                                               |  PC (0x01) midicontrolpy-sw |
                                                               |  song files *.mid, *.conf   |
                                                               |  web status :50118 (LAN)    |
                                                               +-----------------------------+
```

## 2. Components and responsibilities

| Component | Repo | Responsibilities |
|---|---|---|
| Remote (0x03) | `midicontroller-fw` | Song code entry, play / pause / resume / stop / add verse, record start/stop, song list, manual organ-on, shows PC state, pairing to one base, battery/charger, auto power-off, diagnostics, OTA via its bootloader |
| Base (0x02) | `midicontrol-fw` | USB device to the PC (CDC protocol, USB-MIDI, CDC log); USB-MIDI <-> DIN; frame router PC<->remote; BLE central, pairing, link supervision; SSR; indicator bus (send + observe); OLED menu; safety cut-off on PC loss; fault records, watchdogs; bootloader |
| PC (0x01) | `midicontrolpy-sw` | Song store, song list, playback (sections, add verse, pause/resume), recording, organ power policy (on at play, idle-off), stops / default registration, indicator updates, diag polling and store, web page + firmware upload, CLI, firmware updater |
| Shared protocol | `midicontrol-protocol` | Frame format, message types, NAK reasons, diag codes, INFO TLVs; C++ library for both MCUs (PC vendors a copy) |
| Bootloader | `midicontrol-bootloader` | Shared bootloader code; update trailer (magic 0x4D43544C, board ID 2 base / 3 remote, CRC) |
| Indicator bus | existing church hardware | Shared multi-master bus; shows song (4 digits), verse (2 digits), letter, LED; 9-byte frames at 1200 Bd |
| Organ | existing (Opus 910) | MIDI in/out, stop control via SysEx + Program Change |

## 3. Data and control paths (reference)

### 3.1 PC <-> base (USB)

| Item | Behaviour |
|---|---|
| USB IDs | App `ABCD:1234`, bootloader `ABCD:1235` (the PC reports a base found in its bootloader, SYS-FW-08) |
| CDC0 | Protocol frames (SOH...EOT, escaped, CRC-8 MSB-first poly 0x31), frame <= 270 B, payload <= 128 B |
| MIDI | PC playback notes -> base -> DIN (31250 Bd). Display SysEx anchors are stripped by the PC before output. Base echoes indicator-bus frames to the PC as SysEx |
| CDC1 | Text log "MIDIControl log", ring 1536 B, survives resets |
| Reliability | End-to-end ACK/NAK, 3 retries x 500 ms, same PacketId; duplicates re-ACKed, not executed. The base forgets the PC's last PacketId when it declares the PC lost (Q1-8 b, SYS-PC-07). Pending (step 2): NAK BUSY is retried by the sender within the same budget, same PacketId; the base retries only its own requests, forwarded frames are retried by their originator (Q-D-8). The PC drops an older un-ACKed SET_CURRENT when it sends a newer one to the same node, and older un-ACKed SIGNAL_PLAYING / RECORDING / STOPPED / ANCHOR when it sends a newer PLAYING / RECORDING / STOPPED (Q-D-9, Q-D-6, Q-D-7). The PC starts its PacketId counter at a random value per process (Q-D-14). On the remote a STOP always gets a send slot: with all slots busy it evicts the least valuable pending request, which ends with NAK BUSY (RF-11, Q-RF-4) |
| Liveness | PC KEEPALIVE every 2 s; base marks PC lost after 6 s without PC frames, or at once on USB loss. Pending (step 2): the loss is latched; only a received PC frame restores the PC (BF-1, Q-D-11) |
| Polling | PC: GET_STATUS 5 s (2 s timeout); GET_INFO at node-up and every 15 s; DIAG_GET 10 s |

### 3.2 Base <-> remote (BLE)

| Item | Behaviour |
|---|---|
| Roles | Base = central, remote = peripheral, Transparent UART, RNBD350 on both (D10) |
| Security | `SA,2` Just Works on both ends; Just Works accepted by the owner (D21) together with the bonded-base whitelist (D3) |
| Pairing | Base menu "Nastaveni > Sparovat ovladac" plus an explicit pairing action on the remote (D3) |
| Peer filter | Both ends accept frames only from the bonded peer after %SECURED% (base and remote: implemented, SYS-PAIR-04) |
| Reconnect | Base retries every 5 s with bare `C` (module bond) (proposed default, P12); remote re-advertises (30 s retry) |
| PHY / params | TX max (SGA/SGC 0F); 20 ms interval, latency 0, supervision 2.5 s; Coded S8; remote advertises extended on Coded only |
| Link loss | %DISCONNECT% or %ERR_CONN% while connected (RNBD350 never sends %DISCONNECT% for a vanished peer) |
| Keepalive | Base -> remote every 2 s `[pcAlive, baseRssi]`; remote answers KEEPALIVE_ACK `[rssi]`; loss after 6 s. Pending (step 2): a received `pcAlive` 0 ("PC gone", true -> false edge) is the remote's only trigger to reset its player; a BLE drop or an aged-out keepalive does not reset it (Q-D-15) |

### 3.3 Routing

| Message | From -> To | Notes |
|---|---|---|
| PLAY_SONG, STOP_SONG (incl. pause), resume, REPLAY_ANCHOR, GET_SONG_LIST, RECORD_SONG | remote -> PC | Base NAKs UNREACHABLE if the PC is absent |
| SIGNAL_PLAYING / STOPPED / ANCHOR / RECORDING | PC -> remote | Base observes them for its OLED |
| NOW_PLAYING_INFO | PC -> base | Base drives the indicator bus, no ACK |
| SET_CURRENT `[on]` | PC -> base | SSR |
| FW_UPDATE_* | PC -> base, or PC -> remote via base | chunks <= 120 B; base chunks multiple of 8 B |

---

## 4. Requirements

### 4.1 Power-up

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-PWR-01 | Base power-up: SSR off, USB enumerates (2 CDC + MIDI), log banner with git revision and reset cause | implemented | base src/base.cpp:19 (PA4 output, low from static init), main.cpp:273 `Debug::banner()` |
| SYS-PWR-02 | Base power-up with a stored remote: BLE reconnect starts; link up in ~6-8 s **on the first attempt**. A failing first attempt after each boot is a known issue to investigate, not accepted behaviour (proposed default, P1) | implemented (2026-11-12) | link up on the first CE, STREAM_OPEN ~7.4 s after reset. The %CONNECT% + %ERR_CONN% seen right after boot is a connection setup that fails while the remote restarts advertising after losing the old link; the module retries by itself within ~200 ms and it is no longer counted as a loss (link_stats onLost before STREAM_OPEN). Test: basetest test_ble_boot.py, host LinkStats.SetupFailureBeforeStreamOpenIsNotALoss |
| SYS-PWR-03 | Base BLE init failure is non-fatal: USB-MIDI/SSR/indicator keep working; BLE init retried every 60 s | implemented | base main.cpp |
| SYS-PWR-04 | Remote power-on by button; firmware holds the latch; shows "Cekam na zakladnu", then "Cekam na PC" until both are alive | implemented | remote base.cpp, menu.cpp |
| SYS-PWR-05 | Remote: long halt / power loss switches it off; short resets keep it on | implemented | latch |
| SYS-PWR-06 | PC service starts at boot (systemd user unit, linger), Type=notify, WatchdogSec 30, Restart=always 5 s, single instance | implemented, not bench-verified | docker variant pending |
| SYS-PWR-07 | PC service start/restart: send All Notes Off = clean state. Organ power: if the base reports the SSR on (a manual organ-on), leave it on and start the idle-off timer (SYS-ORG-05); otherwise switch it off (SET_CURRENT 0). Do NOT touch the indicator (shared bus) (D14, Q1-1 b 2026-11-14) | implemented, not bench-verified | PC startup_reset.cpp, comm.cpp (step 2, 41f143b): notes part always; power part from the SSR in the first authoritative STATUS_RESP, skipped if a command already claimed the organ. Pending: no SSR report within 12 s -> SET_CURRENT 0 (Q-D-4); the reset runs again after a base reconnect (PCC-3, Q-D-5). Tests test_startup_reset.cpp, comm_q11b_*, comm_pcc3_*. The 2026-11-12 bench run covered the old always-off rule |

### 4.2 Pairing

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-PAIR-01 | Base pairing from base menu, Just Works, 15 s timeout, result on base OLED | implemented | base main.cpp |
| SYS-PAIR-02 | Base connects only to name `MIDIControlRemote`, later only to the stored address | implemented | base |
| SYS-PAIR-03 | Base upgraded from settings v1 must be re-paired once (no trust-on-first-use) | implemented | base settings |
| SYS-PAIR-04 | Remote accepts a connection and frames (incl. OTA, SIGNAL_*) **only from its bonded base**; any other central is refused (D3) | implemented 2026-11-12 | `peerguard::Guard`: frames in and out only on a link whose %CONNECT% MAC is the bond and that is secured; other centrals get K,1 (every 3 s while they stay); module accept list (JC/JA) holds the bond. Residual: the MAC in %CONNECT% is what the module reports, a spoofed MAC still needs our bond key to secure |
| SYS-PAIR-05 | New bond over Coded may go silent after ~4 s; base drops it once (K,1) and reconnects ~5 s later. Workaround accepted (proposed default, P2) | implemented | cause unknown |
| SYS-PAIR-06 | Pairing the remote to a new base is an explicit action on the remote (menu item); without it the remote keeps its current bond (D3) | implemented 2026-11-12 | Nastaveni -> "Sparovat zakl.": 60 s window, accept list cleared, the first central that connects and secures becomes the bond (settings v2 `baseMac`). No trust on first use: a remote upgraded from an older release has no bond and must be paired once from this menu (and from the base menu). Any base in range during the window can take it: switch an old base off first |
| SYS-PAIR-07 | Base reconnect uses the module's last bond (bare `C`); acceptable because both ends whitelist the peer (proposed default, P12) | differs from code (gap) | base bluetooth.cpp `reconnect()` (~:552-561) sends `Z` then `CE,0,<stored addr>` on both PHYs, not bare `C` (SA-7, gap 33) |

### 4.3 Normal use (playback)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-USE-01 | Remote: enter a song code (rule SYS-USE-22: 1-4 digits, value <= 1999, optional letter A-D via Up/Down); OK sends PLAY_SONG; invalid -> "Chybny kod", above 1999 -> "Nejvyse 1999" (Q1-2 a) | implemented, not bench-verified | remote player_screen.cpp, logic song_code (step 2, 5ecea01). Pending: an incoming SIGNAL_PLAYING is still accepted with 1-8 `[0-9A-Z]` (Q-D-17) |
| SYS-USE-02 | PC: song code = file `<CODE>.mid` in the play folder; unknown -> NAK NOT_FOUND; remote shows the error | implemented | PC songs.cpp |
| SYS-USE-03 | No answer after 3 x 500 ms -> remote shows "Bez odpovedi" | implemented | remote |
| SYS-USE-04 | Play: if organ off -> PC switches it on (SET_CURRENT 1, D2), warm-up WarmupMs (3000 ms, interruptible), stops preamble (song stops or default registration, SYS-USE-20), SIGNAL_PLAYING `code`, then SIGNAL_ANCHOR per section | implemented, not bench-verified | PC comm.cpp:1086 `doPlay`; never run on the real organ |
| SYS-USE-05 | Sections come from the display SysEx anchors; blank verse = prelude ("Predehra"), then "Sloka n" | implemented | PC playback.cpp |
| SYS-USE-06 | Each anchor -> NOW_PLAYING_INFO -> base sends one 9-byte frame on the indicator bus | implemented, not bench-verified | no real indicator on the bench |
| SYS-USE-07 | Add verse (REPLAY_ANCHOR): after the current section, replay it once per press (presses accumulate); indicator verse unchanged, no extra hint (proposed default, P4); label "Sloka 2 +1"; NAK NOT_PLAYING if idle; button enabled after the first anchor | implemented | PC playback.cpp:236 |
| SYS-USE-08 | Stop from remote or base -> STOP_SONG -> PC stops, All Notes Off (CC123 x 16 ch), indicator cleared ONLY if it still shows our number (shared bus, D20), SIGNAL_STOPPED. Organ power: see SYS-ORG-04 | partial | PC comm.cpp `displayClear` (step 2, 698ca54, P1-10): clears only if the PC itself set the indicator; whether another master wrote since is not known to the PC (Q-W2-7, gap 27); organ part: gap in SYS-ORG-04 |
| SYS-USE-09 | Pause (STOP_SONG mode 1): real pause. PC halts playback, sends All Notes Off, keeps the song position; SIGNAL to remote/base shows "paused" (D6) | differs from code (gap) | PC dispatch.cpp:100 treats pause as stop |
| SYS-USE-10 | Song end: All Notes Off, indicator cleared only if it still shows our number (D20), SIGNAL_STOPPED. Organ stays on; idle-off timer starts (SYS-ORG-04/05) (D2, D5) | differs from code (gap) | PC comm.cpp `finishPlayback` switches organ off unless KeepPowerOn (default false in config.conf), also when it was switched on manually; indicator part: see SYS-USE-08 (P1-10) |
| SYS-USE-11 | Song list: GET_SONG_LIST with offset, `[hasMore, "a;b;..."]`, paged; play from list; "Zadne pisne" when empty | implemented | remote song_list_screen, PC songs.cpp |
| SYS-USE-12 | New PLAY while playing (or paused) replaces the current song | implemented (paused case depends on SYS-USE-09) | PC comm.cpp:1086 `doPlay` |
| SYS-USE-13 | No tempo change and no transposition, anywhere (D6) | implemented | none present; do not add |
| SYS-USE-14 | Recording is a required feature; see sec. 4.13 SYS-REC (D6, D9) | partial | see SYS-REC |
| SYS-USE-15 | Base OLED: menu Prehraj / Nahraj / Nastaveni; "Odpojeno" overlay without PC; OLED sleeps after 30 s, wakes on input or overlay change | partial | base menu.cpp; "Nahraj" is shown but its callback is empty (menu.cpp:453, UI-1, gap 15). Pending (step 2): NowPlaying is dismissed on PC loss; "Zastavuji" times out after 5 s to the main menu (SM-1, Q-D-12); a failed song list, play or stop shows why: "PC neodpovida", "PC zaneprazdneno", "PC nedostupne" (PM-1, Q-D-13) |
| SYS-USE-16 | Song folder rescan every 60 s | implemented, not bench-verified | PC web_server.cpp `kRescanMs` (status page + songcheck); GET_SONG_LIST and PLAY scan the folder on demand |
| SYS-USE-17 | Indicator shows a song as 4 digit positions, each 0-9 or blank, songs up to **1999** (thousands digit blank or 1, D23) + letter A-D or blank, verse as 2 digit positions (D7, sec. 5) | differs from code (gap) | base `setSong()` (display.cpp:264) caps at 1999 but has no caller; the live path display.cpp:211 `applyNowPlaying` passes any thousands digit (US-6, gap 32). PC `fromSongCode` caps at 1999 since step 2 (bab7e84, Q1-2 a) |
| SYS-USE-18 | Digits are ones-first and right-aligned as the V2 text says; V2's "song 12" example is a typo. Shifted digits in a song file are authoring mistakes (512.mid: verse in the tens position, 513.mid: song shifted left): the PC right-aligns them and songcheck should warn (D24) | implemented (songcheck warning 2026-11-12: songcheck_warns_*) | PC display.cpp |
| SYS-USE-19 | Song without a usable display anchor: PC uses the song code from the file name for the indicator AND flags the file (songcheck warning) (D15) | implemented, not bench-verified | PC playback.cpp:110 fallback; songcheck flags "empty display anchor" / "no display anchors" |
| SYS-USE-20 | Song config without stops: PC sends a configured **default registration** at song start (not "leave stops as they are") (D16) | NOT IMPLEMENTED (gap) | PC organ/stops: no default registration setting |
| SYS-USE-21 | Resume: a resume command from the remote or base continues the paused song at the same spot (same section, same position); SIGNAL_PLAYING sent again (D6) | NOT IMPLEMENTED (gap) | no resume message in PROTOCOL.md, no UI |
| SYS-USE-22 | Song code rule: 1-4 digits, value at most 1999, plus an optional letter A-D. Every unit checks it: the remote on entry and before PLAY_SONG; the PC at every entry point (PLAY_SONG -> NAK INVALID_PAYLOAD, console/CLI, file lookup, song list, stops editor, songcheck); the base on the indicator path. The song list offers only files whose name is a valid code (Q1-2 a, 2026-11-14) | partial | step 2: PC `songs::validCode` (bab7e84, 5c42931), remote 5ecea01; base indicator path not yet (gap 32). Pending: "012" and "12" are two different codes (Q-W2-8). The frame bound MAX_SONG_CODE_LEN stays 8 |
| SYS-USE-23 | The base is a full player: add verse from the base (P1-6) and the whole song list (the 32-song cap goes, or the list pages like the remote's). Until then the base shows a message when the list is cut at 32 (P1-7 a) (Q1-11 b, 2026-11-14) | NOT IMPLEMENTED (gap) | base menu.cpp; waits for the bench stack watermark (RAM 95%), gap 39 |
| SYS-USE-24 | Base NowPlaying screen: a turn of the encoder selects Stop or "Pridat sloku" (add verse); a press runs the selected action (Q-UI2-4, 2026-11-15) | NOT IMPLEMENTED (gap) | base menu.cpp NowPlaying; gap 40 |
| SYS-USE-25 | Base power-on splash closes by itself after 3 s, unless the user already pressed a key: then it stays until Enter (P1-8) | implemented, not bench-verified | base `splashguard::Guard` (step 2, ee890a6), tests SplashGuard.*. Up/Down alone no longer close it (Q-D-1) |

### 4.4 Link loss (BLE)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-LINK-01 | Base detects link loss within ~2.5 s (supervision) or 6 s (keepalive); reconnects every 5 s. A link that stays "connected" and accepted but carries no frame from the remote for 10 s is dropped (`K,1`); if it is still up 5 s later the base restarts its module (once per episode) and auto-reconnect follows (BE-1) | implemented; stale-link recovery not bench-verified | base; stale-link recovery `stalelink::Guard` (`src/logic/stale_link.hpp`, host tests StaleLink.*), `staleLinkWatchdog()` in main.cpp (audit step 3). Off while a remote firmware update is shown (the remote copies its image ~13 s in its bootloader). Kicks and restarts are counted in RAM (`mcStaleKicks`, `mcStaleRestarts`, read by probe; no INFO tag) |
| SYS-LINK-02 | Remote shows "Odpojeno / Cekam na zakladnu" while the base is gone; commands not sent | implemented | remote |
| SYS-LINK-03 | Playback continues on BLE link loss (PC owns playback) | implemented, not bench-verified | PC. Pending (step 2, 83fb028): the remote keeps its player state over a BLE drop and resets it only when the base reports the PC gone (KEEPALIVE `pcAlive` 0) (Q-D-15) |
| SYS-LINK-04 | Frames PC -> remote while the link is down -> NAK UNREACHABLE | implemented | base router.cpp |
| SYS-LINK-05 | Remote ignores RN status text injected by a peer over the data stream (F2/F4); follows from D3 | implemented 2026-11-12 | a %CONNECT% while linked is ignored (F2), "CMD>" not answering our `$$$` is ignored (F4), a refused peer's text is believed only for REBOOT (and DISCONNECT/ERR_CONN after our K,1). Residual: the RN status channel shares the stream, so an accepted (bonded) base could still write status text |
| SYS-LINK-06 | On reconnect, right after the link is secured, the remote asks the PC for now-playing state and updates its screen (D8, P1-9) | partial | step 2 (PC 211a7d7, remote 83fb028): the PC pushes the state instead; the remote never asks. The remote state is "dirty" at PC start, on a ble 0 -> 1 edge in STATUS_RESP and after an un-ACKed SIGNAL_*; while dirty, each STATUS_RESP with ble=1 sends one resync (playing: SIGNAL_PLAYING + the last SIGNAL_ANCHOR; may be playing: SIGNAL_STOPPED; idle and clean: nothing); cleared when all its packets are ACKed; not during an update or a diag pause. Residuals: a remote that reboots and reconnects before the PC sees ble=0 stays idle until the next anchor; one extra SIGNAL_STOPPED after every PC start (Q-D-2). Gap 9 |
| SYS-LINK-07 | GET_STATUS reports BLE "connected" only once the link is secured (proposed default, P5) | implemented 2026-11-12 | `blestatus::linkUsable` (connected, stream open, peer accepted), base router.cpp; same rule as BUSY vs UNREACHABLE |

### 4.5 PC absent / PC crash

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-PC-01 | Base: PC lost -> overlay "Odpojeno / Cekam na PC"; remote learns it from KEEPALIVE `[pcAlive]` and shows "Cekam na PC" | implemented | router.cpp, remote |
| SYS-PC-02 | Remote commands to an absent PC -> NAK UNREACHABLE | implemented | base router |
| SYS-PC-03 | PC lost mid-song (USB gone, PC dead, keepalive timeout): base IMMEDIATELY sends All Notes Off (CC120 + CC123, 16 ch) to DIN AND switches the SSR off (D1). Fires only on the transition PC present -> PC lost; manual use of the base with no PC ever present keeps power (D19). This is the backstop for the PC-owned power policy (D5) | implemented (2026-11-12; DIN half unverified until SYS-NOTE-05; step-2 changes not bench-verified) | base router.cpp logChanges + logic/recovery.hpp PcLossGuard; tests host PcLossGuard.*, hw test_pc_loss.py. Pending (step 2, 64ce11c): the loss is latched, only a received PC packet restores the PC, on a wrap-safe uint32 ms clock (BF-1, Q-D-11) |
| SYS-PC-04 | PC loses USB (base reset): playback stopped, organ state "unknown"; reconnects on USB return | implemented | PC comm.cpp:995 (`SerialLost`) |
| SYS-PC-05 | PC service shutdown (SIGTERM): stop, clear, SIGNAL_STOPPED, organ off. Interactive Ctrl-C may exit without cleanup; SYS-PC-03 and SYS-PWR-07 cover it | implemented | PC comm.cpp:1010 (`Shutdown`) |
| SYS-PC-06 | systemd watchdog ping every 5 s only if worker threads progressed within 10 s | implemented, not bench-verified | PC sd_notify |
| SYS-PC-07 | When the base declares the PC lost, it forgets the PC's last PacketId, so the first frame of a restarted PC is executed, not only re-ACKed (Q1-8 b, 2026-11-14) | implemented, not bench-verified | base (step 2, fd11669). Residual: a PC restart faster than the 6 s loss timeout is not a loss; the PC's random PacketId start (92a5228, pending Q-D-14) leaves a 1/256 collision (gap 34) |

### 4.6 Organ power

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-ORG-01 | SSR switched by SET_CURRENT from the PC or the base menu checkbox "Napajeni varhan"; checkbox follows the real state | implemented, not bench-verified | router.cpp:364, menu.cpp:460; LED on bench, not real SSR. Pending (step 2, PC 2a664d1, ed1bd04): the PC keeps the state it commanded; an authoritative STATUS_RESP that disagrees with an unconfirmed command younger than 30 s triggers up to 3 re-sends, then an error log and the Base status field `organ_power_fault` (PCC-2, Q-D-10) |
| SYS-ORG-02 | SSR off after any base reset | implemented (fw) | base src/base.cpp:19 (PA4 output at static init), main.cpp:290; no hardware pull-down on PA4; the float until GPIO init is accepted (D28) |
| SYS-ORG-03 | Organ on before first note + WarmupMs; if already on, start immediately | implemented, not bench-verified | PC |
| SYS-ORG-04 | Between songs the organ stays on. The PC owns the policy: organ on at play, off after a configurable idle time (default 30 min, D27) (D2, D5) | differs from code (gap) | PC switches off after each song (KeepPowerOn=false) |
| SYS-ORG-05 | Idle-off: PC switches the organ off after X min (default 30, D27) without playback/recording; a paused song is NOT idle (D18); setting in PC config (D5) | partial | step 2 (41f143b): `Config::organIdleOffMs` 30 min, armed only when the startup reset finds the organ on (SYS-PWR-07, Q1-1 b); disarmed by a PC organ command, a song or the base reporting off; restarted by a recording. Not a config.conf key yet; a manual on at runtime and KeepPowerOn=true after a song arm no timer (gap 4) |
| SYS-ORG-06 | SSR drive (LM317L constant-current, <= ~104 mA) must match the installed SSR | open (D12: SSR type unknown, O-4) | hardware |
| SYS-ORG-07 | Manual organ-on from the base menu or the remote: organ stays on (overrides song-end handling) until the next idle timeout; the PC must see the manual state and run its idle timer (D2) | partial | base checkbox exists (menu.cpp:457-464, PC learns it only via the 5 s GET_STATUS poll); remote has no control and shows no organ state; PC switches it off at song end (`finishPlayback`), on stop even when nothing played (`doStop`) and on a failed start; the startup reset leaves it on and arms the idle timer since step 2 (Q1-1 b, SYS-PWR-07); a manual on at runtime arms no timer (gap 4, gap 5). With no PC the base backstop cuts it after 2 h (SYS-IDLE-04) |

### 4.7 Stuck notes / All Notes Off

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-NOTE-01 | PC sends CC123 on all 16 channels at song end, stop, pause, and before each add-verse replay | partial (pause: SYS-USE-09) | PC playback.cpp:152 |
| SYS-NOTE-02 | Base after IWDG/WWDG or fault reset: CC120 + CC123 on 16 ch to DIN, SSR off | implemented (DIN path broken on the bench, SYS-NOTE-05) | base logic/recovery.* |
| SYS-NOTE-03 | Base on PC loss / USB unplug mid-note: All Notes Off + SSR off immediately (D1, same as SYS-PC-03) | implemented (2026-11-12) | base router.cpp (see SYS-PC-03) |
| SYS-NOTE-04 | PC service start/restart: All Notes Off (D14, same as SYS-PWR-07) | implemented (2026-11-12) | PC startup_reset.cpp (see SYS-PWR-07) |
| SYS-NOTE-05 | MIDI over DIN works in both directions (base -> organ for playback and All Notes Off, organ -> base for recording); the bench fault is found and fixed and the link verified before installation (D31) | NOT IMPLEMENTED (gap) | never worked on the bench; cause not traced (HARDWARE-base open item 9) |

### 4.8 Idle / auto-off / remote power

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-IDLE-01 | Remote powers off after 20 min without a key press ("Necinnost: vypnuti") | implemented, not bench-verified | timer never fired on the bench |
| SYS-IDLE-02 | Remote idle clock suspended while playing, during OTA, on USB power | implemented, not bench-verified | remote; during OTA the idle clock is reset by every FW packet (link.cpp:167 `Power::touch()`), not suspended (P-TG-2). Pending (step 2, 74ed6d4, 83fb028): playing counts only while the PC is alive (link up and no PC-loss report), so after a drop without reconnect the auto-off still runs (RF-12, Q-D-16) |
| SYS-IDLE-03 | Remote power: easy wins only (OLED sleep, WFI, BLE module/peripheral sleep where trivial); no stop-mode redesign required (D17) | partial | OLED sleep + WFI done; BLE module / peripheral sleep not reviewed |
| SYS-IDLE-04 | Base: OLED sleep after 30 s. Organ backstop: if the SSR is on and no valid packet has come from the PC for 2 h, the base sends All Notes Off and switches the SSR off; any received PC packet restarts the 2 h (Q1-1 c, 2026-11-14). Organ idle-off with a PC present is the PC's (SYS-ORG-05) | implemented, not bench-verified | OLED sleep: implemented. Backstop: base `routercore::SsrBackstop` (step 2, 976f788), tests Backstop.*. With the 2 s KEEPALIVE it acts only when the PC is absent. Pending: switching the SSR on also restarts the 2 h; hand playing with no PC is cut after 2 h (Q-D-3) |

### 4.9 Battery (remote)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-BAT-01 | 2P NCR18650B (~7 Ah); BQ24195 charge 2048 mA, term 256 mA, 12 h timer; input limit from Type-C Rp / D+D- | implemented, not bench-verified | confirmed by the owner (D28); the pack may change later |
| SYS-BAT-02 | SOC: 47 ms samples, trimmed mean, EMA, hysteresis; 4180 mV = 100 %, 3300 mV = 0 % | implemented | remote logic/battery_soc |
| SYS-BAT-03 | Critical: 5 consecutive 1 s means < 3300 mV -> "Slaba baterie", shutdown, diag slot saved | implemented, not bench-verified | remote battery.cpp |
| SYS-BAT-04 | Remote cannot switch off or read its button while USB is plugged in | implemented | by hardware |
| SYS-BAT-05 | Battery % on the status page | implemented | PC status |

### 4.10 Firmware update

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-FW-01 | Only `<name>-update.bin` with 24 B trailer; board ID checked in browser, PC and device | implemented | PC firmware_image, bootloader |
| SYS-FW-02 | Base update over USB: ENTER_BOOTLOADER, bootloader CDC (ABCD:1235), first 2 KB written last; ~11 s | implemented | base bootloader |
| SYS-FW-03 | A power cut during the vector-page commit must not hang the base bootloader (F4); fixed before deployment (proposed default, P6) | implemented 2026-11-13 | `boot::ecc::handleNmi`: ECC double error in the app region -> erase that page + vector page, reset; base stays in its bootloader (USB), remote releases PA1 |
| SYS-FW-04 | An image without a valid trailer must not boot (F5); fixed before deployment (proposed default, P6) | implemented 2026-11-13 | `boot::appBootable` accepts only `Check::Ok`; a plain ELF over SWD no longer starts (flash `-full.hex` / `-update.hex`) |
| SYS-FW-05 | Base bootloader entry: TAMP BKP0R magic or encoder held at power-on; progress on OLED | implemented | base bootloader |
| SYS-FW-06 | Remote OTA via base: staged in W25Q32, CRC check, bootloader copies (3 tries per boot) | implemented | ~75 s 1M, 2 min 32 s Coded S8 (68.8 kB) |
| SYS-FW-07 | Remote accepts OTA only from its bonded base (D3) | implemented 2026-11-12 | OTA frames pass the same guard as SYS-PAIR-04 |
| SYS-FW-08 | PC refuses play/control commands during a firmware update, and detects a base left in its bootloader (offers re-flash) (proposed default, P7; P1-13, P1-14) | partial | PC (step 2): one claim gate refuses an update while a Play is queued, warming up or playing (766c596, P1-14; CLI/socket b3340d2); a Play during an update is not refused (Q-W2-5). A base left in its USB bootloader (ABCD:1235) is detected and reported: Base status field `in_bootloader`, error log and console advice "update base <file>" (385f536, P1-13); no automatic re-flash (Q-W2-6). Gap 22 |
| SYS-FW-09 | Firmware upload from the web page (POST <= 512 KiB -> 202/409/422), no authentication; church LAN is trusted (assumption, D4) | implemented | PC web_server |

### 4.11 Errors and diagnostics

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-DIAG-01 | Base: fault record in no-init RAM, loop watchdog 1000 ms, IWDG ~2 s, reported at next boot | implemented | base fault.*, wdg; debug builds: `stack` command on the log CDC prints the stack high-water mark (step 2, 3237615) |
| SYS-DIAG-02 | Remote: 32-record RAM ring, flash slots for fault/stall/error shutdown; loop watchdog 2000 ms, IWDG ~4 s; a fault powers the remote off | implemented | remote diag.*, fault.* |
| SYS-DIAG-03 | PC polls DIAG_GET every 10 s, stores JSONL 1 MiB x 3; pauses during remote OTA | implemented | PC diag_poller |
| SYS-DIAG-04 | PROTOCOL.md lists all diag codes (incl. 17 BLE_MODULE_REBOOT, 18 BLE_ERR_CONN), KEEPALIVE byte 1 (base RSSI) and the correct CRC name (non-reflected, poly 0x31) (proposed default, P8) | implemented | midicontrol-protocol PROTOCOL.md:32 (CRC), :179 (KEEPALIVE byte 1), :263-264 (diag 17/18), protocol 6ce5716 |
| SYS-DIAG-05 | Link metrics: RSSI both ends, link losses, conn params in INFO TLVs 0x14/0x15/0x17 | implemented | router.cpp, remote |
| SYS-DIAG-06 | User-visible texts are Czech without diacritics | implemented | both fw |
| SYS-DIAG-07 | Test key mailbox (shipped in the release image, both fw): a probe writes `0xA5<<24 \| seq<<8 \| key` to RAM word `mcTestKey` with a non-halting memory write; the main loop clears it, presses the key exactly like a real one and echoes the word to `mcTestKeyAck`; an invalid word is acked with top byte `0xEE`. Keys: base Up 0, Down 1, Enter 4 (`GUI::Key`); remote `Keyboard::Key` index 0..15 (digits 1-9 = 0..8, MULTI 9, ZERO 10, UP 11, DOWN 12, LEFT 13, RIGHT 14, OK 15; `Power::touch()` included). Both words are in .bss: a reset clears them. Take the addresses from the flashed ELF (`nm`), never from an older build | implemented, not bench-verified | base main.cpp `testKeyPoll()`, remote base.cpp `Keyboard::testKeyPoll()` (audit step 3) |

### 4.12 Status web page

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-WEB-01 | HTTP on 0.0.0.0:50118; page polls JSON every 1 s; 3 s timeout -> "bez spojeni" | implemented, not bench-verified | PC |
| SYS-WEB-02 | "Prehled" (Czech): health, now playing, stops; "Pokrocile" (English): debug | implemented, not bench-verified | PC web_page |
| SYS-WEB-03 | Controller dot green only if STATUS_RESP <= 15 s old | implemented, not bench-verified | PC status |
| SYS-WEB-04 | Stops editor writes `<CODE>.conf` | implemented, not bench-verified | PC stops_file |
| SYS-WEB-05 | No authentication, no Host check: accepted. Assumption: the church LAN is trusted; page stays open (D4) | implemented | PC http.cpp |

### 4.13 Recording (new group, D6, D9)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-REC-01 | Recording is started and stopped **from the base only** (menu); the remote has no recording control. PC confirms with SIGNAL_RECORDING; base (and remote, display only) show "recording" (D29) | partial | base menu "Nahraj" exists; RECORD_SONG / SIGNAL_RECORDING "reserved" in PROTOCOL.md |
| SYS-REC-02 | PC records the organ's MIDI from DIN (via base USB-MIDI in) into a new song file in the song store | partial | PC `record` spawns arecordmidi (comm.cpp:887); DIN-in path never verified |
| SYS-REC-03 | Every indicator-bus frame seen during recording (from the base or any manual controller) is written into the file at its time position as the same embedded display SysEx the player parses today | NOT IMPLEMENTED (gap) | base echoes bus frames as SysEx to USB; PC does not merge them into the file |
| SYS-REC-04 | A recorded file plays back like any other song: sections from the recorded anchors, songcheck-clean | NOT IMPLEMENTED (gap) | depends on SYS-REC-03 |
| SYS-REC-05 | Starting a recording switches the organ on (same as play, SYS-ORG-03) and resets the idle timer; recording is not idle (D29) | NOT IMPLEMENTED (gap) | PC |
| SYS-REC-06 | A recording gets a name with NO song number, from its start date and time (suggested `REC_YYYY-MM-DD_HHMM.mid`); the owner renames it to a song code later (D29) | NOT IMPLEMENTED (gap) | PC |
| SYS-REC-07 | The silence between recording start and the first note is cut, so playback starts at the first note. Stop changes and other non-note events do not count as the first note (D29) | NOT IMPLEMENTED (gap) | PC |
| SYS-REC-08 | The organist sets the stops while recording. The PC parses the stop changes from the recorded MIDI and writes `<name>.conf` from them (the registration in effect at the first note); those initial stop changes are taken out of the leading silence. Stop changes later in the song stay in the file (D29) | NOT IMPLEMENTED (gap) | PC organ/stops, stops_file |

### 4.14 Shared indicator bus (new group, D9, D13, D14)

The indicator bus is **multi-master**: manual controllers in the church can send any song number at any time.
The base is one writer among several.

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-BUS-01 | Base observes (receives) every frame on the bus and never assumes it is the only writer | partial | base display.cpp receives frames and echoes them to the PC as SysEx; no further use |
| SYS-BUS-02 | A foreign song number on the bus (manual controller) is mirrored on the base OLED and on the remote; playback is NOT touched (D13) | NOT IMPLEMENTED (gap) | base fw (OLED + forward to remote), remote fw (display), protocol (message) |
| SYS-BUS-03 | While recording, foreign frames are recorded into the MIDI file (D9, SYS-REC-03) | NOT IMPLEMENTED (gap) | see SYS-REC-03 |
| SYS-BUS-04 | The base writes to the bus only on NOW_PLAYING_INFO from the PC; PC start/restart does not write to the bus (D14) | implemented | base display.cpp, PC |
| SYS-BUS-05 | Before transmitting, the base waits until the bus has been quiet for a short time (a few byte times; suggested 25 ms = 3 bytes at 1200 Bd) and is not receiving a frame; then it sends (D25) | implemented 2026-11-12 | `indicator::QuietBus` (25 ms, base display.cpp); real-bus check pending |
| SYS-BUS-06 | Base hot-plug sense of the bus (PC13); no frames sent while the bus is absent | implemented, not bench-verified | base display.cpp; bench has no indicator |
| SYS-BUS-07 | On stop / song end the indicator is cleared only if it still shows the number we sent; if another master wrote since, leave it (D20). The PC never clears a number it did not set itself (P1-10) | partial | PC part done (step 2, 698ca54): clear only after the PC set the indicator. "Another master wrote since" is not checked (needs the base, Q-W2-7), gap 27 |
| SYS-BUS-08 | A received frame is accepted only if it is 9 bytes and starts with the `0xB0` flag; anything else is dropped and does not change the base's state (D26) | implemented 2026-11-12 | `indicator::frameAccepted` (length + flag); rejected frames counted in `display::rxRejected` |

### 4.15 Documentation (new group)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-DOC-01 | Indicator bus protocol and song code set documented from the owner's V2 protocol files (D7), sec. 5 | implemented | `midicontrol-protocol/INDICATOR-BUS.md` |
| SYS-DOC-02 | `midicontrol-fw` IMPLEMENTATION_PROMPT.md marked obsolete (proposed default, P11) | implemented | OBSOLETE banner at the top of midicontrol-fw IMPLEMENTATION_PROMPT.md (c0d26cf) |
| SYS-DOC-03 | Doc inconsistencies resolved from code (RSSI poll 10 vs 11 s, ST-Link serials, ConfirmScreen language, JSON schema 1 vs 2, upload head limit 4 vs 8 KiB) (proposed default, P9) | implemented | RSSI 11 s (sec. 8, remote TODO.md, midicontrolpy-sw docs/FAULTS.md c45c17a), serials agree in both fw CLAUDE.md/HARDWARE docs, ConfirmScreen Ano/Ne (remote CLAUDE.md), `/api/status` schema 2 (status-web-and-service.md), 8 KiB head+body / 4 KiB body (PC CLAUDE.md, status-web-and-service.md) |
| SYS-DOC-04 | SysEx manufacturer ID 0x4D kept; the display format is fixed by existing church hardware (proposed default, P10) | implemented | - |
| SYS-DOC-05 | Hardware docs: firmware pin map + bench are authoritative; KiCad repos are historical (D11). Schematics showing RN4870 are pre-adapter (D10) | implemented | docs/HARDWARE.md in both fw repos |

---

## 5. Church indicator / display bus protocol

Full reference: **`midicontrol-protocol/INDICATOR-BUS.md`** (English translation of the owner's original V2 document,
kept as `midicontrol-protocol/docs/indicator-bus/Komunikacni_protokol_svetelneho_ukazatele.odt`, plus the mapping to
NOW_PLAYING_INFO and the display SysEx). In the firmware repos it is under `lib/protocol/`. Summary:

- **Electrical.** RJ11: pin 2 data, pin 4 GND, pin 5 supply 12-14 V from the indicator; pins 1/3/6 unused. One data
  line for receive and transmit, about 14 V. Isolated interface: TX through PC817 -> PNP (BC807) -> diode -> 200-300 ohm
  onto the line; RX through 4k7 -> PC817. The circuit inverts, so the MCU sees a plain 3.3 V UART.
- **UART.** 1200 Bd (8N1 in the firmware), about 75 ms per frame.
- **Frame.** 9 bytes: `B0` flag, verse ones, verse tens, song ones, tens, hundreds, thousands, letter (`0A`-`0D` = A-D),
  LED (`20` red, `21` yellow, `22` green, `23` blue). Upper nibble `E` in any byte = that field blank (LED off).
- **Multi-master.** Every controller (ours and the manual ones) transmits on the same line. V2 defines no arbitration
  rule; the base waits for a short quiet bus before sending (D25, SYS-BUS-05) and accepts only frames that start
  with `B0` (D26).
- **Colour numbering differs.** SysEx and NOW_PLAYING_INFO use 1 red, 2 green, 3 blue, 4 yellow. The bus uses `20` red,
  `21` yellow, `22` green, `23` blue. Convert by table.
- **Song code set (D7).** 4 digit positions (0-9 or blank), letter A-D or blank. Songs go up to 1999 only
  (thousands digit blank or 1, D23). The PC and the remote enforce the song code rule (SYS-USE-22, Q1-2 a); the base indicator path does not yet (gap 32). All four LED colours exist on the board (D26).
- **Digit order (D24).** The V2 text rule holds: ones first, numbers right-aligned. V2's second example ("song 12")
  is a typo. Song files with shifted digits (512, 513) are authoring mistakes.

---

## 6. Gaps (required but not implemented)

Work list. Close a row by changing the requirement status in sec. 4 and deleting the row here.

| # | Requirement(s) | Gap | Repo(s) to change |
|---|---|---|---|
| 1 | SYS-PC-03, SYS-NOTE-03 | CLOSED 2026-11-12 (PcLossGuard, test_pc_loss.py; DIN half waits for gap 28): Base: on PC loss (USB gone / keepalive timeout) send All Notes Off + SSR off immediately (D1) | midicontrol-fw |
| 2 | SYS-PWR-07, SYS-NOTE-04 | CLOSED 2026-11-12 (startup_reset.cpp, test_startup_reset.cpp): PC start/restart: All Notes Off + organ off, indicator untouched (D14). Power rule changed by Q1-1 b (a manual on stays on), see SYS-PWR-07 | midicontrolpy-sw |
| 3 | SYS-ORG-04, SYS-USE-10 | Organ stays on between songs; no off at song end/stop (D2, D5) | midicontrolpy-sw |
| 4 | SYS-ORG-05 | Configurable idle-off timer on the PC (D5). Step 2: 30 min timer only for an organ found on at PC start or reconnect (41f143b); not a config key yet; a manual on at runtime and KeepPowerOn after a song arm no timer | midicontrolpy-sw |
| 5 | SYS-ORG-07 | Manual organ-on from base/remote overrides until the next idle timeout; PC learns manual state; remote control. Today the PC switches a manual on off at stop (also with nothing playing), song end and failed start (US-2/3/4, Q-US-2); the startup reset keeps it since step 2 (Q1-1 b); the remote has no organ control or display (US-5, routing Q-US-3); a manual base-on with no PC is cut by the 2 h backstop since step 2 (Q1-1 c, SYS-IDLE-04; was US-17, Q-US-4) | midicontrolpy-sw, midicontroller-fw, midicontrol-protocol (if a new message), midicontrol-fw |
| 6 | SYS-PAIR-04, SYS-FW-07 | CLOSED 2026-11-12 (`peerguard::Guard`, `test_remote_guard.py`, `PeerGuard.*`): Remote accepts connection/frames/OTA only from the bonded base (D3) | midicontroller-fw |
| 7 | SYS-PAIR-06 | CLOSED 2026-11-12 (menu "Sparovat zakl.", `test_pairing_window_*`): Explicit "pair new base" action on the remote (D3) | midicontroller-fw |
| 8 | SYS-LINK-05 | CLOSED 2026-11-12 (`test_remote_guard.py`, `test_remote_status_rx.py`, `StatusFramer.*`): Remote ignores RN status text injected by a peer (F2/F4) | midicontroller-fw |
| 9 | SYS-LINK-06 | Remote requests now-playing after the link is secured (D8, P1-9). Step 2 has the PC push the state on ble 0 -> 1 instead (211a7d7, 83fb028); the owner decides whether the push meets D8 | midicontroller-fw, midicontrolpy-sw (answer), midicontrol-protocol (if a new message) |
| 10 | SYS-LINK-07 | CLOSED 2026-11-12 (`test_cdc_local.py::test_status_ble_only_when_secured`, `BleLinkUsable.*`): GET_STATUS BLE "connected" only when secured (P5) | midicontrol-fw |
| 11 | SYS-PWR-02 | CLOSED 2026-11-12: first BLE attempt after base boot fails; cause = remote advertising restart collides with the first connection setup, now a non-counted setup failure, link up on the first CE (P1) | midicontrol-fw |
| 12 | SYS-USE-09, SYS-NOTE-01 | Real pause: halt, All Notes Off, keep position (D6) | midicontrolpy-sw, midicontroller-fw, midicontrol-fw (OLED) |
| 13 | SYS-USE-21 | Resume at the same spot; resume message + UI (D6) | midicontrol-protocol, midicontrolpy-sw, midicontroller-fw, midicontrol-fw |
| 14 | SYS-USE-20 | Default registration for songs without stops (D16) | midicontrolpy-sw |
| 15 | SYS-USE-14, SYS-USE-15, SYS-REC-01 | Record start/stop from the base; RECORD_SONG / SIGNAL_RECORDING defined (D9, D29). The base "Nahraj" item is an empty stub today (menu.cpp:109, :453; UI-1, P-UI-1) | midicontrol-protocol, midicontrolpy-sw, midicontrol-fw |
| 16 | SYS-REC-02 | Record organ MIDI from DIN into a new song file; verify DIN-in path | midicontrolpy-sw, midicontrol-fw |
| 17 | SYS-REC-03, SYS-REC-04, SYS-BUS-03 | Embed bus frames seen during recording as display SysEx; recorded file plays back with anchors (D9) | midicontrolpy-sw (+ midicontrol-fw if the echo is incomplete) |
| 18 | SYS-BUS-01, SYS-BUS-02 | Observe the shared bus; mirror foreign song number on base OLED and remote, playback untouched (D13) | midicontrol-fw, midicontroller-fw, midicontrol-protocol |
| 19 | SYS-IDLE-03 | Remote power easy wins (BLE module / peripheral sleep) (D17) | midicontroller-fw |
| 22 | SYS-FW-08 | PC refuses play/control during update; finds base stuck in bootloader (P7). Done: update refused while a song plays or a Play is claimed (b3340d2, 766c596); base in bootloader detected and reported (385f536). Open: refuse Play during an update (Q-W2-5); re-flash offer is console advice only (Q-W2-6) | midicontrolpy-sw |
| 23 | SYS-DIAG-04 | CLOSED 2026-11-14 (protocol 6ce5716): PROTOCOL.md: diag 17/18, KEEPALIVE byte 1, CRC name (P8) | midicontrol-protocol |
| 24 | SYS-BUS-08 | CLOSED 2026-11-12 (`test_display.py::test_rx_frame_flag_check`, `IndicatorFrame.*`): check the `0xB0` flag on received indicator frames | midicontrol-fw |
| 25 | SYS-DOC-02 | CLOSED 2026-11-14 (midicontrol-fw c0d26cf): Mark IMPLEMENTATION_PROMPT.md obsolete (P11) | midicontrol-fw |
| 26 | SYS-DOC-03 | CLOSED 2026-11-14 (midicontrolpy-sw c45c17a): Resolve doc inconsistencies from code (P9) | midicontrol-fw, midicontroller-fw, midicontrolpy-sw |
| 27 | SYS-BUS-07, SYS-USE-08, SYS-USE-10 | Clear indicator on stop/end only if it still shows our number (D20). PC part (clear only what the PC set, P1-10) done in step 2 (698ca54); open: "no other master wrote since" needs the base to know the last writer (Q-W2-7) | midicontrol-fw, midicontrolpy-sw |
| 28 | SYS-NOTE-05 | Find and fix the DIN MIDI fault (cable / ATtiny port direction / MIDI_TX_VCC_ON / TX polarity); verify both directions before installation (D31) | midicontrol-fw, midicontrol-hw (ATtiny fw?) |
| 29 | SYS-BUS-05 | CLOSED 2026-11-12 (`test_display.py::test_quiet_bus_holds_tx`, `QuietBus.*`): wait for a short quiet bus before transmitting (D25); real-bus check in midicontrol-fw/TODO.md | midicontrol-fw |
| 30 | SYS-REC-05..08 | Recording: organ on, date/time file name, trim leading silence, stops -> `.conf` (D29) | midicontrolpy-sw |
| 31 | SYS-USE-18 | CLOSED 2026-11-12 (songcheck_warns_*): songcheck warns on shifted display digits (D24) | midicontrolpy-sw |
| 32 | SYS-USE-17, SYS-USE-22 | Enforce the 1999 song cap on the live indicator path: `setSong()` caps but has no caller, display.cpp:211 `applyNowPlaying` passes 2000-9999 (US-6). PC `fromSongCode` fixed in step 2 (bab7e84) | midicontrol-fw |
| 33 | SYS-PAIR-07, sec. 3.2 Reconnect | Spec says reconnect with bare `C` (P12); code sends `Z` + `CE,0,<stored addr>` (stricter; bare `C` never connected to the remote's Coded-only advertiser on the bench, learning 2026-11-07). Owner decides which is the rule (SA-7, Q-SA-3) | midicontrol-fw (or this spec) |
| 34 | sec. 3.1 Reliability, SYS-PC-07 | Owner chose Q1-8 b: the base forgets the PC's PacketId on PC loss (step 2, fd11669); the PC starts its PacketIds at a random value (92a5228, pending Q-D-14). Residual: a PC restart faster than the 6 s loss timeout keeps the old slot; 1/256 chance that the first frame is ACKed but not executed. A full fix needs a protocol change (session id) (SA-4, Q-SA-2) | midicontrol-fw, midicontrolpy-sw, midicontrol-protocol (if a session id) |
| 35 | SYS-ORG-02 | PA4 (SSR) level during reset, bootloader stay and OTA never measured; no pull-down (D28) (SB-3, Q-SB-1) | midicontrol-fw, midicontrol-hw |
| 36 | sec. 3.2 Link | Remote module stuck in command mode while connected and the base is quiet is not detected (late `CMD>` after a `$$$` timeout, lost `END`; remote bluetooth.cpp:343-391); an unconnected module recovers via the RST re-init (RD-1, Q-RD-5) | midicontroller-fw |
| 37 | sec. 2 Remote row, sec. 3.3 Routing vs SYS-REC-01 | Spec contradiction: sec. 2 gives the remote record start/stop and sec. 3.3 routes RECORD_SONG remote -> PC; SYS-REC-01 / D29 say recording is base-only (D29 is the later owner decision, 2026-11-12: remote recording not wanted), so sec. 2 / 3.3 likely only need rewording; no remote recording is to be built. Owner rewords. Not part of this gap: remote manual organ-on is required (SYS-ORG-07, gap 5); resume is the missing message of gap 13 (SA-2, Q-SA-1, Q-RD-1) | this spec |
| 38 | SYS-USE-01, SYS-USE-22, sec. 8 Song code | Resolved in the spec by Q1-2 a (SYS-USE-22); PC and remote enforce it in step 2. Open: base indicator path (gap 32); protocol `constants.hpp` comment still says `[0-9A-Z]` (code change in midicontrol-protocol); remote accepts any 1-8 `[0-9A-Z]` in an incoming SIGNAL_PLAYING (Q-D-17) (US-7, UI-5) | midicontrol-protocol, midicontrol-fw, midicontroller-fw (if Q-D-17) |
| 39 | SYS-USE-23 | Base full player (Q1-11 b): add verse from the base (P1-6), song list without the 32 cap or paged; message when the cap is hit until then (P1-7 a). Not started: waits for the bench stack watermark (base RAM 95%) | midicontrol-fw |
| 40 | SYS-USE-24 | Base NowPlaying: encoder turn selects Stop / "Pridat sloku", press runs it (Q-UI2-4) | midicontrol-fw |

---

## 7. Hardware at a glance

Details: `docs/HARDWARE.md` in `midicontrol-fw` (base) and `midicontroller-fw` (remote). **The firmware pin
map and the bench are authoritative**; `midicontrol-hw` / `midicontroller-hw` are historical reference (D11).
BLE in ALL units (base and remote): **RNBD350 on an RN4870-footprint adapter board**: UART and RST as on the RN4870.
The RNBD350 has **no MODE pin, so MODE is not wired** (D22); the MCU MODE outputs (base PA0, remote PB9) drive
nothing. Schematics showing an RN4870 are pre-adapter (D10).

| Item | Base (`midicontrol-fw`) | Remote (`midicontroller-fw`) |
|---|---|---|
| MCU | STM32G431CBT6 (Cortex-M4F, 128 KB flash, 32 KB SRAM), 144 MHz from 48 MHz HSE | STM32G071CBT6 (Cortex-M0+, 128 KB flash, 36 KB SRAM), 64 MHz from HSI16 |
| BLE | RNBD350 on adapter; USART2 PA2/PA3 115200, RST PA1 (PA0 MODE output unconnected, D22) | RNBD350 on adapter; USART2 PA2/PA3 |
| Display / input | SH1106 OLED (I2C), EC11 encoder with switch, WS2812B LEDs (4 front, 6 rear) | SH1106 OLED (I2C 0x3C), 5x3 key matrix + OK + power button |
| Organ / bus I/O | 2 MIDI DIN ports (TX buffer, 6N138 opto RX, ports switched by an ATtiny1607 with its own fw); indicator bus: isolated single-wire UART (PC817) + presence sense PC13; SSR drive PA4 -> PC817 -> LM317L current source -> 500 mA fuse | - |
| Power | USB VBUS 5 V; MC34063 buck to 3V3; supercap-backed 5VL rail | 2P NCR18650B; BQ24195 charger (I2C 0x6B); AP2112K-3.3 behind a latch (PA1 hold) |
| USB | USB-C full-speed device: CDC protocol + USB-MIDI + CDC log | USB-C power/charging only (UCPD CC sense, BQ24195 D+/D-) |
| Storage | internal flash (BL 32 K / app 94 K / settings 2 K) | W25Q32 4 MB SPI NOR (OTA staging, diag slots); internal flash BL 16 K / app 110 K / settings 2 K |
| Debug | SWD 2x3 header (no VTref unless JP2 bridged), STLINK-V3 | SWD 5-pin header, ST-LINK/V2 |
| Open HW points | SSR type unknown (D12, O-4); DIN link never worked on the bench (SYS-NOTE-05, must be fixed) | PB13 change of 2026-10-19 undocumented |

---

## 8. Timing and limits

| Item | Value |
|---|---|
| ACK timeout / retries | 500 ms x 3, same PacketId; also the PC's `sendReliable` defaults (P1-18); firmware-update steps use their own values (FW update row) |
| KEEPALIVE period / loss | 2 s / 6 s |
| BLE supervision | 2.5 s, interval 20 ms, latency 0 |
| Base BLE reconnect / remote advertise retry | 5 s / 30 s |
| Unsecured-link watchdog | B after 3 s, K,1 after +3 s |
| Pairing timeout | base search 15 s; remote "Sparovat zakl." window 60 s, refused-peer retry 3 s |
| Base BLE init retry | 60 s |
| RSSI poll | base 7 s, remote 11 s (not 10: avoids beating with the PC DIAG poll); a reading older than 25 s counts as stale (both) |
| Max payload / frame | 128 B / 270 B |
| Song code | 1-4 digits, value <= 1999, optional A-D (SYS-USE-22); a frame carries at most 8 bytes (MAX_SONG_CODE_LEN) |
| FW chunk | <= 120 B; base multiple of 8 B |
| PC GET_STATUS / GET_INFO / DIAG | 5 s (2 s timeout) / 15 s / 10 s |
| PC warm-up | WarmupMs 3000 ms |
| Organ idle-off | 30 min default, configurable on the PC (D27); step 2: `Config::organIdleOffMs`, not yet a config key, armed only for an organ found on at PC start (gap 4) |
| Base organ backstop | 2 h with the SSR on and no PC packet (Q1-1 c, SYS-IDLE-04) |
| PC startup reset | power part waits up to 12 s for an SSR report (pending, Q-D-4) |
| PC organ reconcile | unconfirmed command younger than 30 s, up to 3 re-sends (pending, Q-D-10) |
| Stops | AllOffDelayMs 100, StopDelayMs 20 default, per stop |
| Verse gap | VerseGapMs 0 |
| Remote idle off / power button | 20 min / hold 5 s |
| Key debounce | remote 20 ms; base encoder/keys polled every 10 ms |
| Battery cut-off | 3300 mV, 5 x 1 s |
| Charge | 2048 mA, term 256 mA, 12 h timer |
| Watchdogs | base loop 1000 ms / IWDG ~2 s; remote loop 2000 ms / IWDG ~4 s |
| OLED sleep | base 30 s, remote 30 s |
| Indicator bus | 1200 Bd, 9 B frames |
| MIDI DIN | 31250 Bd |
| Web | port 50118, poll 1 s, fetch timeout 3 s (stops editor 5 s), upload <= 512 KiB with a 60 s deadline; other requests <= 8 KiB, bodies <= 4 KiB, 3 s deadline; song folder rescan 60 s |
| systemd | WatchdogSec 30, ping 5 s, RestartSec 5 |
| Song rescan | 60 s |
| Diag store | JSONL 1 MiB x 3 |
| OTA duration | base ~11 s; remote ~75 s (1M) / 2 min 32 s (Coded S8, 68.8 kB) |
| BLE range | unverified (O-6) |
| FW update (PC) | START 15 s remote / 3 s base x3; DATA 1.5 s x8 per chunk; END 5 s x3; OLED progress stall 20 s, result shown 10 s |
| Battery warning (remote) | below 3450 mV, 100 mV hysteresis |
| Song list request | 3 s timeout (base and remote) |
| OLED messages | base "stopped" screen 1.5 s, base "Zastavuji" timeout 5 s (pending, Q-D-12), base splash 3 s (stays until Enter once a key was pressed, P1-8); remote message 2.5 s |
| PC status page | recent-problem window 10 min; STATUS_RESP counts as fresh for 15 s |

## 9. Glossary (Czech UI terms)

| Term | Meaning |
|---|---|
| Prehraj / Nahraj / Nastaveni | Play / Record / Settings (base menu) |
| Sparovat ovladac / Sparuj ovladac | Pair the remote (menu item / hint) |
| Napajeni varhan | Organ power (checkbox) |
| Odpojeno / Nepripojeno | Not synonyms. Odpojeno: disconnected, overlay title (base: no PC; remote: no base or no PC), also remote battery page "USB: Odpojeno". Nepripojeno: not connected, remote message when a song command or list cannot be sent |
| Cekam na PC / Cekam na zakladnu | Waiting for the PC / for the base |
| Ovladac nenalezen | Remote not found (pairing) |
| Bez odpovedi | No answer (retries exhausted) |
| Chybny kod | Invalid song code |
| Nejvyse 1999 | At most 1999 (remote, song code above the cap, Q1-2 a) |
| Pridat sloku | Add verse (base NowPlaying action, SYS-USE-24, not implemented yet) |
| Zadne pisne | No songs |
| Predehra | Prelude |
| Sloka n / Sloka n +k | Verse n / verse n replayed k extra times (PC label; the OLEDs show "Sloka: n" / "Sloka: n (xk)", "Predehra (xk)") |
| Necinnost: vypnuti | Inactivity: switching off |
| Slaba baterie | Weak battery; on the remote OLED shown only at the 3300 mV cut-off under "Vypnuti" (the web says "slabá" for the 3450 mV warning, "vybitá" for cut-off; Q-TG-6) |
| Instalace | Installing (OTA copy) |
| Ano / Ne / Zpet | Yes / No / Back |
| Prehled / Pokrocile | Overview / Advanced (web tabs; the page shows "Přehled" / "Pokročilé", web text uses diacritics) |
| bez spojeni | no connection (web; the header shows "bez spojení s přehrávačem", no connection to the player) |
| rejstrik(y) | organ stop(s) |
| tabule | church indicator board (no UI uses the word: base menu says "Displej", web Advanced "Church display"; Q-TG-3) |
| zakladna / ovladac | base / remote |
| Prehravac / Hlavni menu | Player screen / main menu (remote) |
| Prehravam / Spoustim / Zastavuji / Zastaveno | Playing / starting / stopping / stopped (player state) |
| Nehraje se | Nothing is playing (PC NAK NOT_PLAYING) |
| Neznama <kod> | Unknown song code (PC NAK NOT_FOUND) |
| PC nedostupne / PC zaneprazdneno / Odmitnuto | PC unreachable / busy / refused (NAK reasons) |
| PC neodpovida | PC did not answer (base: song list within 3 s; since step 2 also play and stop after the retries, pending Q-D-13; the remote says "Bez odpovedi") |
| Zkuste znovu | Try again (command could not be queued) |
| Pisne / Nacitani... | Song list / loading |
| Parovani / Hledam ovladac... / Sparovano / Nesparovano / Parovani selhalo / Cas vyprsel / Zbyva n s | Pairing screens |
| Sparovat zakl. | Pair a new base (remote menu, opens the 60 s pairing window, SYS-PAIR-06; abbreviated, Q-TG-2) |
| Sparovat? (Kod nnnnnn) | Confirm pairing (passkey, information only) |
| Vypnuti / Drzte 5 s | Switching off / hold 5 s (remote power button) |
| Aktualizace / Prijem dat / Prenos do ovladace / Instalace ovladace / Chyba aktualizace / Chyba, opakujte / Nevypinejte | Firmware update screens (the last two from the base bootloader) |
| Baterie: Napeti / Uroven / Nabiji / Nabito / Nenabiji / Vybiji | Battery page (remote) |
| Verze FW: Revize / Vetev / Datum / Cas | Firmware version page |
| Modul: OK / OK, bez RST / neodpovida / mlci | BLE module self-test, Bluetooth page: OK / OK but the reset line does not work / no reply / silent (Q-TG-4) |
| Ident. MIDI / LED / Displej | Base settings: MIDI identification, status LED, church board page |
| Stisk: Nastaveni | Press: settings (hint on the remote's Odpojeno overlay) |
| MIDIControl BASE / CONTROLLER | Power-on splash, English product name (Q-TG-1) |

## 10. Open items

| # | Item | Source | Blocks |
|---|---|---|---|
| O-1 | RESOLVED D23: songs up to 1999 | D23 | SYS-USE-17 |
| O-2 | RESOLVED D24: V2 text rule holds; example 2 is a typo; shifted files are authoring mistakes | D24 | SYS-USE-18 |
| O-3 | RESOLVED D27: idle-off default 30 min | D27 | SYS-ORG-04/05 |
| O-4 | SSR type installed (bare-LED input vs 3-32 VDC input): unknown for now (owner, 2026-11-12) | D12, Q19 | SYS-ORG-06 |
| O-5 | RESOLVED D28: no PA4 pull-down; float accepted | D28 | SYS-ORG-02 |
| O-6 | Church range walk not done yet (owner, 2026-11-12) | Q11, P3 | - |
| O-7 | RESOLVED D31: the DIN fault must be found and fixed before installation | D31 | SYS-NOTE-05 |
| O-8 | RESOLVED D28: 2P NCR18650B, 2048 mA confirmed; the pack may change later | D28 | SYS-BAT-01 |
| O-9 | RESOLVED D29: base only, date/time name, organ on, leading silence cut, stops parsed to `.conf` | D29 | SYS-REC-01, -05..-08 |
| O-10 | RESOLVED D25: wait for a short quiet bus before transmitting | D25 | SYS-BUS-05 |
| O-11 | RESOLVED D18: a paused song is NOT idle | D18 | SYS-ORG-05, SYS-USE-09 |
| O-12 | RESOLVED D21: Just Works accepted (with the D3 bonded-base whitelist) | D21 | SYS-PAIR-01 |
| O-13 | RESOLVED D19: cut-off only on the transition PC present -> PC lost; manual use with no PC ever present keeps power | D19 | SYS-PC-03, SYS-ORG-07 |
| O-14 | Stop mapping for Opus 910 (ProgramOffset -1, octal numbering) still unverified. For recorded songs the stops come from the recording (D29); content of the default registration for other songs still open | Q25, D16 | SYS-USE-20, SYS-REC-08 |
| O-15 | RESOLVED D30: P1-P12 delegated to the developers | D30 | marked rows |
| O-16 | Step-2 behaviour implemented on `audit/step2-review` without an owner decision (marked "pending" above): Q-D-1..Q-D-17, Q-W2-1..Q-W2-8, Q-RF-4. List: `audit/findings/step2/phase-d-docs.md` | audit step 2 | rows marked pending |
