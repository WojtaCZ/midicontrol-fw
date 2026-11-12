<!-- COPY of midicontrolpy-sw/docs/SYSTEM.md. Do not edit here: edit the main copy, then run midicontrolpy-sw/tools/sync-system-doc.sh -->

# MIDIcontrol system specification

Version 2026-11-12. Owner decisions D1-D31 applied (binding). Items marked **(proposed default)** come from
P1-P12: technical choices the owner delegated to the developers (D30); change them only with a reason. Code reference: base `midicontrol-fw` 6a68acc, remote
`midicontroller-fw` 7539b85, PC `midicontrolpy-sw` main 8a7ed0f (the docker branch is not covered).
Wire protocol: `midicontrol-protocol/PROTOCOL.md`.

> **How to use this document**
>
> 1. The main copy lives in `midicontrolpy-sw/docs/SYSTEM.md`. The other repos hold identical copies,
>    updated by `midicontrolpy-sw/tools/sync-system-doc.sh`. Edit only the main copy, then run the script.
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
| USB IDs | App `ABCD:1234`, bootloader `ABCD:1235` |
| CDC0 | Protocol frames (SOH...EOT, escaped, CRC-8 MSB-first poly 0x31), frame <= 270 B, payload <= 128 B |
| MIDI | PC playback notes -> base -> DIN (31250 Bd). Display SysEx anchors are stripped by the PC before output. Base echoes indicator-bus frames to the PC as SysEx |
| CDC1 | Text log "MIDIControl log", ring 1536 B, survives resets |
| Reliability | End-to-end ACK/NAK, 3 retries x 500 ms, same PacketId; duplicates re-ACKed, not executed |
| Liveness | PC KEEPALIVE every 2 s; base marks PC lost after 6 s without PC frames, or at once on USB loss |
| Polling | PC: GET_STATUS 5 s (2 s timeout); GET_INFO at node-up and every 15 s; DIAG_GET 10 s |

### 3.2 Base <-> remote (BLE)

| Item | Behaviour |
|---|---|
| Roles | Base = central, remote = peripheral, Transparent UART, RNBD350 on both (D10) |
| Security | `SA,2` Just Works on both ends; Just Works accepted by the owner (D21) together with the bonded-base whitelist (D3) |
| Pairing | Base menu "Nastaveni > Sparovat ovladac" plus an explicit pairing action on the remote (D3) |
| Peer filter | Both ends accept frames only from the bonded peer after %SECURED% (base: implemented; remote: gap) |
| Reconnect | Base retries every 5 s with bare `C` (module bond) (proposed default, P12); remote re-advertises (30 s retry) |
| PHY / params | TX max (SGA/SGC 0F); 20 ms interval, latency 0, supervision 2.5 s; Coded S8; remote advertises extended on Coded only |
| Link loss | %DISCONNECT% or %ERR_CONN% while connected (RNBD350 never sends %DISCONNECT% for a vanished peer) |
| Keepalive | Base -> remote every 2 s `[pcAlive, baseRssi]`; remote answers KEEPALIVE_ACK `[rssi]`; loss after 6 s |

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
| SYS-PWR-01 | Base power-up: SSR off, USB enumerates (2 CDC + MIDI), log banner with git revision and reset cause | implemented | base main.cpp:290 |
| SYS-PWR-02 | Base power-up with a stored remote: BLE reconnect starts; link up in ~6-8 s **on the first attempt**. A failing first attempt after each boot is a known issue to investigate, not accepted behaviour (proposed default, P1) | partial | base bluetooth.cpp: first attempt ends in %ERR_CONN% (link_losses starts at 1) |
| SYS-PWR-03 | Base BLE init failure is non-fatal: USB-MIDI/SSR/indicator keep working; BLE init retried every 60 s | implemented | base main.cpp |
| SYS-PWR-04 | Remote power-on by button; firmware holds the latch; shows "Cekam na zakladnu", then "Cekam na PC" until both are alive | implemented | remote base.cpp, menu.cpp |
| SYS-PWR-05 | Remote: long halt / power loss switches it off; short resets keep it on | implemented | latch |
| SYS-PWR-06 | PC service starts at boot (systemd user unit, linger), Type=notify, WatchdogSec 30, Restart=always 5 s, single instance | implemented, not bench-verified | docker variant pending |
| SYS-PWR-07 | PC service start/restart: send All Notes Off and switch organ power off (SET_CURRENT 0) = clean state. Do NOT touch the indicator (shared bus) (D14) | differs from code (gap) | PC comm.cpp: no action at start, organ state "unknown" until first STATUS_RESP |

### 4.2 Pairing

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-PAIR-01 | Base pairing from base menu, Just Works, 15 s timeout, result on base OLED | implemented | base main.cpp |
| SYS-PAIR-02 | Base connects only to name `MIDIControlRemote`, later only to the stored address | implemented | base |
| SYS-PAIR-03 | Base upgraded from settings v1 must be re-paired once (no trust-on-first-use) | implemented | base settings |
| SYS-PAIR-04 | Remote accepts a connection and frames (incl. OTA, SIGNAL_*) **only from its bonded base**; any other central is refused (D3) | NOT IMPLEMENTED (gap) | remote accepts any central (REVIEW I5/R35) |
| SYS-PAIR-05 | New bond over Coded may go silent after ~4 s; base drops it once (K,1) and reconnects ~5 s later. Workaround accepted (proposed default, P2) | implemented | cause unknown |
| SYS-PAIR-06 | Pairing the remote to a new base is an explicit action on the remote (menu item); without it the remote keeps its current bond (D3) | NOT IMPLEMENTED (gap) | remote has no pairing UI |
| SYS-PAIR-07 | Base reconnect uses the module's last bond (bare `C`); acceptable because both ends whitelist the peer (proposed default, P12) | implemented | depends on SYS-PAIR-04 |

### 4.3 Normal use (playback)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-USE-01 | Remote: enter 1-8 chars `[0-9A-Z]` (digits + letter A-D via Up/Down); OK sends PLAY_SONG; invalid -> "Chybny kod" | implemented | remote player_screen.cpp |
| SYS-USE-02 | PC: song code = file `<CODE>.mid` in the play folder; unknown -> NAK NOT_FOUND; remote shows the error | implemented | PC songs.cpp |
| SYS-USE-03 | No answer after 3 x 500 ms -> remote shows "Bez odpovedi" | implemented | remote |
| SYS-USE-04 | Play: if organ off -> PC switches it on (SET_CURRENT 1, D2), warm-up WarmupMs (3000 ms, interruptible), stops preamble (song stops or default registration, SYS-USE-20), SIGNAL_PLAYING `code`, then SIGNAL_ANCHOR per section | implemented, not bench-verified | PC comm.cpp:1037; never run on the real organ |
| SYS-USE-05 | Sections come from the display SysEx anchors; blank verse = prelude ("Predehra"), then "Sloka n" | implemented | PC playback.cpp |
| SYS-USE-06 | Each anchor -> NOW_PLAYING_INFO -> base sends one 9-byte frame on the indicator bus | implemented, not bench-verified | no real indicator on the bench |
| SYS-USE-07 | Add verse (REPLAY_ANCHOR): after the current section, replay it once per press (presses accumulate); indicator verse unchanged, no extra hint (proposed default, P4); label "Sloka 2 +1"; NAK NOT_PLAYING if idle; button enabled after the first anchor | implemented | PC playback.cpp:236 |
| SYS-USE-08 | Stop from remote or base -> STOP_SONG -> PC stops, All Notes Off (CC123 x 16 ch), indicator cleared ONLY if it still shows our number (shared bus, D20), SIGNAL_STOPPED. Organ power: see SYS-ORG-04 | implemented (organ part: gap in SYS-ORG-04) | PC comm.cpp:1143 |
| SYS-USE-09 | Pause (STOP_SONG mode 1): real pause. PC halts playback, sends All Notes Off, keeps the song position; SIGNAL to remote/base shows "paused" (D6) | differs from code (gap) | PC dispatch.cpp:100 treats pause as stop |
| SYS-USE-10 | Song end: All Notes Off, indicator cleared only if it still shows our number (D20), SIGNAL_STOPPED. Organ stays on; idle-off timer starts (SYS-ORG-04/05) (D2, D5) | differs from code (gap) | PC comm.cpp:1132 switches organ off unless KeepPowerOn, also when it was switched on manually |
| SYS-USE-11 | Song list: GET_SONG_LIST with offset, `[hasMore, "a;b;..."]`, paged; play from list; "Zadne pisne" when empty | implemented | remote song_list_screen, PC songs.cpp |
| SYS-USE-12 | New PLAY while playing (or paused) replaces the current song | implemented (paused case depends on SYS-USE-09) | PC comm.cpp:1038 |
| SYS-USE-13 | No tempo change and no transposition, anywhere (D6) | implemented | none present; do not add |
| SYS-USE-14 | Recording is a required feature; see sec. 4.13 SYS-REC (D6, D9) | partial | see SYS-REC |
| SYS-USE-15 | Base OLED: menu Prehraj / Nahraj / Nastaveni; "Odpojeno" overlay without PC; OLED sleeps after 30 s, wakes on input or overlay change | implemented | base menu.cpp |
| SYS-USE-16 | Song folder rescan every 60 s | implemented, not bench-verified | PC |
| SYS-USE-17 | Indicator shows a song as 4 digit positions, each 0-9 or blank, songs up to **1999** (thousands digit blank or 1, D23) + letter A-D or blank, verse as 2 digit positions (D7, sec. 5) | implemented | base `setSong()` caps at 1999 |
| SYS-USE-18 | Digits are ones-first and right-aligned as the V2 text says; V2's "song 12" example is a typo. Shifted digits in a song file are authoring mistakes (512.mid: verse in the tens position, 513.mid: song shifted left): the PC right-aligns them and songcheck should warn (D24) | implemented (songcheck warning: verify) | PC display.cpp |
| SYS-USE-19 | Song without a usable display anchor: PC uses the song code from the file name for the indicator AND flags the file (songcheck warning) (D15) | implemented, not bench-verified | PC playback.cpp:110 fallback; songcheck flags "empty display anchor" / "no display anchors" |
| SYS-USE-20 | Song config without stops: PC sends a configured **default registration** at song start (not "leave stops as they are") (D16) | NOT IMPLEMENTED (gap) | PC organ/stops: no default registration setting |
| SYS-USE-21 | Resume: a resume command from the remote or base continues the paused song at the same spot (same section, same position); SIGNAL_PLAYING sent again (D6) | NOT IMPLEMENTED (gap) | no resume message in PROTOCOL.md, no UI |

### 4.4 Link loss (BLE)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-LINK-01 | Base detects link loss within ~2.5 s (supervision) or 6 s (keepalive); reconnects every 5 s | implemented | base |
| SYS-LINK-02 | Remote shows "Odpojeno / Cekam na zakladnu" while the base is gone; commands not sent | implemented | remote |
| SYS-LINK-03 | Playback continues on BLE link loss (PC owns playback) | implemented, not bench-verified | PC |
| SYS-LINK-04 | Frames PC -> remote while the link is down -> NAK UNREACHABLE | implemented | base router.cpp |
| SYS-LINK-05 | Remote ignores RN status text injected by a peer over the data stream (F2/F4); follows from D3 | NOT IMPLEMENTED (gap) | remote bluetooth.cpp |
| SYS-LINK-06 | On reconnect, right after the link is secured, the remote asks the PC for now-playing state and updates its screen (D8) | NOT IMPLEMENTED (gap) | remote waits for the next SIGNAL_* |
| SYS-LINK-07 | GET_STATUS reports BLE "connected" only once the link is secured (proposed default, P5) | differs from code (gap) | base reports connected from %CONNECT% |

### 4.5 PC absent / PC crash

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-PC-01 | Base: PC lost -> overlay "Odpojeno / Cekam na PC"; remote learns it from KEEPALIVE `[pcAlive]` and shows "Cekam na PC" | implemented | router.cpp, remote |
| SYS-PC-02 | Remote commands to an absent PC -> NAK UNREACHABLE | implemented | base router |
| SYS-PC-03 | PC lost mid-song (USB gone, PC dead, keepalive timeout): base IMMEDIATELY sends All Notes Off (CC120 + CC123, 16 ch) to DIN AND switches the SSR off (D1). Fires only on the transition PC present -> PC lost; manual use of the base with no PC ever present keeps power (D19). This is the backstop for the PC-owned power policy (D5) | NOT IMPLEMENTED (gap) | base router.cpp has no handler |
| SYS-PC-04 | PC loses USB (base reset): playback stopped, organ state "unknown"; reconnects on USB return | implemented | PC comm.cpp:990 |
| SYS-PC-05 | PC service shutdown (SIGTERM): stop, clear, SIGNAL_STOPPED, organ off. Interactive Ctrl-C may exit without cleanup; SYS-PC-03 and SYS-PWR-07 cover it | implemented | PC comm.cpp:998 |
| SYS-PC-06 | systemd watchdog ping every 5 s only if worker threads progressed within 10 s | implemented, not bench-verified | PC sd_notify |

### 4.6 Organ power

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-ORG-01 | SSR switched by SET_CURRENT from the PC or the base menu checkbox "Napajeni varhan"; checkbox follows the real state | implemented, not bench-verified | router.cpp:364, menu.cpp:460; LED on bench, not real SSR |
| SYS-ORG-02 | SSR off after any base reset | implemented (fw) | base main.cpp:290; no hardware pull-down on PA4; the float until GPIO init is accepted (D28) |
| SYS-ORG-03 | Organ on before first note + WarmupMs; if already on, start immediately | implemented, not bench-verified | PC |
| SYS-ORG-04 | Between songs the organ stays on. The PC owns the policy: organ on at play, off after a configurable idle time (default 30 min, D27) (D2, D5) | differs from code (gap) | PC switches off after each song (KeepPowerOn=false) |
| SYS-ORG-05 | Idle-off: PC switches the organ off after X min (default 30, D27) without playback/recording; a paused song is NOT idle (D18); setting in PC config (D5) | NOT IMPLEMENTED (gap) | with KeepPowerOn=true it never goes off |
| SYS-ORG-06 | SSR drive (LM317L constant-current, <= ~104 mA) must match the installed SSR | open (D12: SSR type unknown, O-4) | hardware |
| SYS-ORG-07 | Manual organ-on from the base menu or the remote: organ stays on (overrides song-end handling) until the next idle timeout; the PC must see the manual state and run its idle timer (D2) | partial | base checkbox exists; remote has no control; PC switches it off at song end and has no idle timer |

### 4.7 Stuck notes / All Notes Off

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-NOTE-01 | PC sends CC123 on all 16 channels at song end, stop, pause, and before each add-verse replay | partial (pause: SYS-USE-09) | PC playback.cpp:152 |
| SYS-NOTE-02 | Base after IWDG/WWDG or fault reset: CC120 + CC123 on 16 ch to DIN, SSR off | implemented (DIN path broken on the bench, SYS-NOTE-05) | base logic/recovery.* |
| SYS-NOTE-03 | Base on PC loss / USB unplug mid-note: All Notes Off + SSR off immediately (D1, same as SYS-PC-03) | NOT IMPLEMENTED (gap) | - |
| SYS-NOTE-04 | PC service start/restart: All Notes Off (D14, same as SYS-PWR-07) | differs from code (gap) | PC |
| SYS-NOTE-05 | MIDI over DIN works in both directions (base -> organ for playback and All Notes Off, organ -> base for recording); the bench fault is found and fixed and the link verified before installation (D31) | NOT IMPLEMENTED (gap) | never worked on the bench; cause not traced (HARDWARE-base open item 9) |

### 4.8 Idle / auto-off / remote power

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-IDLE-01 | Remote powers off after 20 min without a key press ("Necinnost: vypnuti") | implemented, not bench-verified | timer never fired on the bench |
| SYS-IDLE-02 | Remote idle clock suspended while playing, during OTA, on USB power | implemented, not bench-verified | remote |
| SYS-IDLE-03 | Remote power: easy wins only (OLED sleep, WFI, BLE module/peripheral sleep where trivial); no stop-mode redesign required (D17) | partial | OLED sleep + WFI done; BLE module / peripheral sleep not reviewed |
| SYS-IDLE-04 | Base: OLED sleep after 30 s; no other idle behaviour on the base (organ idle-off is the PC's, SYS-ORG-05) | implemented | base |

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
| SYS-FW-03 | A power cut during the vector-page commit must not hang the base bootloader (F4); fixed before deployment (proposed default, P6) | NOT IMPLEMENTED (gap) | SWD needed today |
| SYS-FW-04 | An image without a valid trailer must not boot (F5); fixed before deployment (proposed default, P6) | differs from code (gap) | boots unless BOOT_REQUIRE_TRAILER=1 |
| SYS-FW-05 | Base bootloader entry: TAMP BKP0R magic or encoder held at power-on; progress on OLED | implemented | base bootloader |
| SYS-FW-06 | Remote OTA via base: staged in W25Q32, CRC check, bootloader copies (3 tries per boot) | implemented | ~75 s 1M, 2 min 32 s Coded S8 (68.8 kB) |
| SYS-FW-07 | Remote accepts OTA only from its bonded base (D3) | NOT IMPLEMENTED (gap) | same work as SYS-PAIR-04 |
| SYS-FW-08 | PC refuses play/control commands during a firmware update, and detects a base left in its bootloader (offers re-flash) (proposed default, P7) | NOT IMPLEMENTED (gap) | PC |
| SYS-FW-09 | Firmware upload from the web page (POST <= 512 KiB -> 202/409/422), no authentication; church LAN is trusted (assumption, D4) | implemented | PC web_server |

### 4.11 Errors and diagnostics

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-DIAG-01 | Base: fault record in no-init RAM, loop watchdog 1000 ms, IWDG ~2 s, reported at next boot | implemented | base fault.*, wdg |
| SYS-DIAG-02 | Remote: 32-record RAM ring, flash slots for fault/stall/error shutdown; loop watchdog 2000 ms, IWDG ~4 s; a fault powers the remote off | implemented | remote diag.*, fault.* |
| SYS-DIAG-03 | PC polls DIAG_GET every 10 s, stores JSONL 1 MiB x 3; pauses during remote OTA | implemented | PC diag_poller |
| SYS-DIAG-04 | PROTOCOL.md lists all diag codes (incl. 17 BLE_MODULE_REBOOT, 18 BLE_ERR_CONN), KEEPALIVE byte 1 (base RSSI) and the correct CRC name (non-reflected, poly 0x31) (proposed default, P8) | differs from code (gap) | spec text lags the code |
| SYS-DIAG-05 | Link metrics: RSSI both ends, link losses, conn params in INFO TLVs 0x14/0x15/0x17 | implemented | router.cpp, remote |
| SYS-DIAG-06 | User-visible texts are Czech without diacritics | implemented | both fw |

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
| SYS-BUS-05 | Before transmitting, the base waits until the bus has been quiet for a short time (a few byte times; suggested 25 ms = 3 bytes at 1200 Bd) and is not receiving a frame; then it sends (D25) | NOT IMPLEMENTED (gap) | base sends whenever it has a change |
| SYS-BUS-06 | Base hot-plug sense of the bus (PC13); no frames sent while the bus is absent | implemented, not bench-verified | base display.cpp; bench has no indicator |
| SYS-BUS-07 | On stop / song end the indicator is cleared only if it still shows the number we sent; if another master wrote since, leave it (D20) | differs from code (gap, verify) | clearing today is unconditional (SYS-USE-08/10) |
| SYS-BUS-08 | A received frame is accepted only if it is 9 bytes and starts with the `0xB0` flag; anything else is dropped and does not change the base's state (D26) | partial (gap) | base display.cpp checks the length only, not the flag |

### 4.15 Documentation (new group)

| ID | Expected behaviour | Status vs code | Where / note |
|---|---|---|---|
| SYS-DOC-01 | Indicator bus protocol and song code set documented from the owner's V2 protocol files (D7), sec. 5 | implemented | `midicontrol-protocol/INDICATOR-BUS.md` |
| SYS-DOC-02 | `midicontrol-fw` IMPLEMENTATION_PROMPT.md marked obsolete (proposed default, P11) | NOT IMPLEMENTED (gap) | contradicts PROTOCOL.md |
| SYS-DOC-03 | Doc inconsistencies resolved from code (RSSI poll 10 vs 11 s, ST-Link serials, ConfirmScreen language, JSON schema 1 vs 2, upload head limit 4 vs 8 KiB) (proposed default, P9) | NOT IMPLEMENTED (gap) | CLAUDE.md / docs in fw and PC repos |
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
  (thousands digit blank or 1, D23); the base `setSong()` caps there. All four LED colours exist on the board (D26).
- **Digit order (D24).** The V2 text rule holds: ones first, numbers right-aligned. V2's second example ("song 12")
  is a typo. Song files with shifted digits (512, 513) are authoring mistakes.

---

## 6. Gaps (required but not implemented)

Work list. Close a row by changing the requirement status in sec. 4 and deleting the row here.

| # | Requirement(s) | Gap | Repo(s) to change |
|---|---|---|---|
| 1 | SYS-PC-03, SYS-NOTE-03 | Base: on PC loss (USB gone / keepalive timeout) send All Notes Off + SSR off immediately (D1) | midicontrol-fw |
| 2 | SYS-PWR-07, SYS-NOTE-04 | PC start/restart: All Notes Off + organ off, indicator untouched (D14) | midicontrolpy-sw |
| 3 | SYS-ORG-04, SYS-USE-10 | Organ stays on between songs; no off at song end/stop (D2, D5) | midicontrolpy-sw |
| 4 | SYS-ORG-05 | Configurable idle-off timer on the PC (D5) | midicontrolpy-sw |
| 5 | SYS-ORG-07 | Manual organ-on from base/remote overrides until the next idle timeout; PC learns manual state; remote control | midicontrolpy-sw, midicontroller-fw, midicontrol-protocol (if a new message), midicontrol-fw |
| 6 | SYS-PAIR-04, SYS-FW-07 | Remote accepts connection/frames/OTA only from the bonded base (D3) | midicontroller-fw |
| 7 | SYS-PAIR-06 | Explicit "pair new base" action on the remote (D3) | midicontroller-fw |
| 8 | SYS-LINK-05 | Remote ignores RN status text injected by a peer (F2/F4) | midicontroller-fw |
| 9 | SYS-LINK-06 | Remote requests now-playing after the link is secured (D8) | midicontroller-fw, midicontrolpy-sw (answer), midicontrol-protocol (if a new message) |
| 10 | SYS-LINK-07 | GET_STATUS BLE "connected" only when secured (P5) | midicontrol-fw |
| 11 | SYS-PWR-02 | First BLE attempt after base boot fails; investigate and fix (P1) | midicontrol-fw |
| 12 | SYS-USE-09, SYS-NOTE-01 | Real pause: halt, All Notes Off, keep position (D6) | midicontrolpy-sw, midicontroller-fw, midicontrol-fw (OLED) |
| 13 | SYS-USE-21 | Resume at the same spot; resume message + UI (D6) | midicontrol-protocol, midicontrolpy-sw, midicontroller-fw, midicontrol-fw |
| 14 | SYS-USE-20 | Default registration for songs without stops (D16) | midicontrolpy-sw |
| 15 | SYS-USE-14, SYS-REC-01 | Record start/stop from the base; RECORD_SONG / SIGNAL_RECORDING defined (D9, D29) | midicontrol-protocol, midicontrolpy-sw, midicontrol-fw |
| 16 | SYS-REC-02 | Record organ MIDI from DIN into a new song file; verify DIN-in path | midicontrolpy-sw, midicontrol-fw |
| 17 | SYS-REC-03, SYS-REC-04, SYS-BUS-03 | Embed bus frames seen during recording as display SysEx; recorded file plays back with anchors (D9) | midicontrolpy-sw (+ midicontrol-fw if the echo is incomplete) |
| 18 | SYS-BUS-01, SYS-BUS-02 | Observe the shared bus; mirror foreign song number on base OLED and remote, playback untouched (D13) | midicontrol-fw, midicontroller-fw, midicontrol-protocol |
| 19 | SYS-IDLE-03 | Remote power easy wins (BLE module / peripheral sleep) (D17) | midicontroller-fw |
| 20 | SYS-FW-03 | Bootloader must survive power cut during vector-page commit (F4, P6) | midicontrol-bootloader, midicontrol-fw (bootloader) |
| 21 | SYS-FW-04 | Image without trailer must not boot (F5, P6) | midicontrol-bootloader, midicontrol-fw, midicontroller-fw (build flag) |
| 22 | SYS-FW-08 | PC refuses play/control during update; finds base stuck in bootloader (P7) | midicontrolpy-sw |
| 23 | SYS-DIAG-04 | PROTOCOL.md: diag 17/18, KEEPALIVE byte 1, CRC name (P8) | midicontrol-protocol |
| 24 | SYS-BUS-08 | Check the `0xB0` flag on received indicator frames | midicontrol-fw |
| 25 | SYS-DOC-02 | Mark IMPLEMENTATION_PROMPT.md obsolete (P11) | midicontrol-fw |
| 26 | SYS-DOC-03 | Resolve doc inconsistencies from code (P9) | midicontrol-fw, midicontroller-fw, midicontrolpy-sw |
| 27 | SYS-BUS-07, SYS-USE-08, SYS-USE-10 | Clear indicator on stop/end only if it still shows our number (D20) | midicontrol-fw, midicontrolpy-sw |
| 28 | SYS-NOTE-05 | Find and fix the DIN MIDI fault (cable / ATtiny port direction / MIDI_TX_VCC_ON / TX polarity); verify both directions before installation (D31) | midicontrol-fw, midicontrol-hw (ATtiny fw?) |
| 29 | SYS-BUS-05 | Wait for a short quiet bus before transmitting (D25) | midicontrol-fw |
| 30 | SYS-REC-05..08 | Recording: organ on, date/time file name, trim leading silence, stops -> `.conf` (D29) | midicontrolpy-sw |
| 31 | SYS-USE-18 | songcheck warns on shifted display digits (D24) | midicontrolpy-sw |

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
| ACK timeout / retries | 500 ms x 3, same PacketId |
| KEEPALIVE period / loss | 2 s / 6 s |
| BLE supervision | 2.5 s, interval 20 ms, latency 0 |
| Base BLE reconnect / remote advertise retry | 5 s / 30 s |
| Unsecured-link watchdog | B after 3 s, K,1 after +3 s |
| Pairing timeout | 15 s |
| Base BLE init retry | 60 s |
| RSSI poll | base ~7 s, remote 10-11 s (SYS-DOC-03) |
| Max payload / frame | 128 B / 270 B |
| Song code | 1-8 chars `[0-9A-Z]` (real set: sec. 5) |
| FW chunk | <= 120 B; base multiple of 8 B |
| PC GET_STATUS / GET_INFO / DIAG | 5 s (2 s timeout) / 15 s / 10 s |
| PC warm-up | WarmupMs 3000 ms |
| Organ idle-off | 30 min default, configurable on the PC (D27) |
| Stops | AllOffDelayMs 100, StopDelayMs per stop |
| Verse gap | VerseGapMs 0 |
| Remote idle off / power button | 20 min / hold 5 s |
| Key debounce | 20 ms |
| Battery cut-off | 3300 mV, 5 x 1 s |
| Charge | 2048 mA, term 256 mA, 12 h timer |
| Watchdogs | base loop 1000 ms / IWDG ~2 s; remote loop 2000 ms / IWDG ~4 s |
| OLED sleep (base) | 30 s |
| Indicator bus | 1200 Bd, 9 B frames |
| MIDI DIN | 31250 Bd |
| Web | port 50118, poll 1 s, fetch timeout 3 s, upload <= 512 KiB |
| systemd | WatchdogSec 30, ping 5 s, RestartSec 5 |
| Song rescan | 60 s |
| Diag store | JSONL 1 MiB x 3 |
| OTA duration | base ~11 s; remote ~75 s (1M) / 2 min 32 s (Coded S8, 68.8 kB) |
| BLE range | unverified (O-6) |

## 9. Glossary (Czech UI terms)

| Term | Meaning |
|---|---|
| Prehraj / Nahraj / Nastaveni | Play / Record / Settings (base menu) |
| Sparovat ovladac / Sparuj ovladac | Pair the remote (menu item / hint) |
| Napajeni varhan | Organ power (checkbox) |
| Odpojeno / Nepripojeno | Disconnected / Not connected |
| Cekam na PC / Cekam na zakladnu | Waiting for the PC / for the base |
| Ovladac nenalezen | Remote not found (pairing) |
| Bez odpovedi | No answer (retries exhausted) |
| Chybny kod | Invalid song code |
| Zadne pisne | No songs |
| Predehra | Prelude |
| Sloka n / Sloka n +k | Verse n / verse n replayed k extra times |
| Necinnost: vypnuti | Inactivity: switching off |
| Slaba baterie | Weak battery |
| Instalace | Installing (OTA copy) |
| Ano / Ne / Zpet | Yes / No / Back |
| Prehled / Pokrocile | Overview / Advanced (web tabs) |
| bez spojeni | no connection (web) |
| rejstrik(y) | organ stop(s) |
| tabule | church indicator board |
| zakladna / ovladac | base / remote |

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
