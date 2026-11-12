# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Read first: system spec and hardware

`docs/SYSTEM.md` is the owner's specification of what the whole MIDIcontrol system (PC software, base, remote,
bootloader, protocol) is expected to do, with requirement IDs, owner decisions and a gap list. Its main copy lives in
`midicontrolpy-sw/docs/SYSTEM.md`; the copies in the other repos are kept identical by
`midicontrolpy-sw/tools/sync-system-doc.sh` (`--check` reports drift). Edit only the main copy. When the code and the spec
disagree, the spec says what is wanted: ask the owner instead of changing the spec to match the code.
`docs/HARDWARE-base.md` / `docs/HARDWARE-remote.md` (or `docs/HARDWARE.md` in the matching firmware repo) describe the
base (STM32G431) and remote (STM32G071) hardware. The firmware pin maps are authoritative; the KiCad repos are historical.

This repo holds the MAIN copy of the base hardware description, `docs/HARDWARE.md` (copied elsewhere as `HARDWARE-base.md`).

## Project Overview

STM32G431CBTx (Cortex-M4F, 144 MHz) firmware for the MIDIcontrol **base station**: USB-MIDI card for the PC player, packet router between the PC (USB-CDC) and the handheld remote (BLE, Microchip **RN4870**), SSR driver for organ power, church indicator (7-segment display) driver, OLED (SH1106 via I2C) with an encoder menu, and RGB status LEDs. Written in C++23 with no exceptions; no heap use in interrupt handlers (fixed-size lock-free ring buffers, `inc/ringbuffer.hpp`).

The packet protocol is defined in **`lib/protocol/PROTOCOL.md`** (shared `midicontrol-protocol` submodule) — that file is authoritative. `readme.md` documents the display SysEx format (`F0 4D 43 ...`), which is also the anchor format in the song files.

## Build Commands

```bash
# Configure + build (app, bootloader via ExternalProject, merged hex)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake [-DCMAKE_BUILD_TYPE=Release]
cmake --build build -j

# Flash bootloader + app in one go (ST-Link, OpenOCD)
openocd -f openocd.cfg -c "program build/midicontrol-fw-full.hex verify reset exit"
```

Or use the container task runner: `mcu build`, `mcu buildDebug`, `mcu flash` (merged hex), `mcu flashApp` (app only), `mcu flashBootloader`, `mcu size` (see `.mcu-profile.json`).

Build outputs in `build/`:
- `midicontrol-fw.elf/.hex/.bin` — app as linked
- `midicontrol-fw-update.bin` — app + image trailer, sent by the PC updater (`FW_UPDATE_*`)
- `midicontrol-fw-update.hex` — the same at 0x08008000, 0xFF-filled to 0x0801F800. **Flash this (or the merged hex), not the plain `.hex`**: a stale trailer left in flash makes the bootloader refuse to start the app
- `bootloader/base-bootloader.hex` — bootloader (`bootloader/`, its own CMake project; `-DBUILD_BOOTLOADER=OFF` skips it)
- `midicontrol-fw-full.hex` — bootloader + `-update.hex` merged

Requires `arm-none-eabi-gcc`. A git metadata header (`inc/git.hpp`) is auto-generated on each build via `cmake/git.cmake`. There are no unit tests in this project; hardware tests (USB-CDC, both ST-Links, logic analyzer) live in `tools/basetest/` (README there).

**Screen images**: `tools/screens/export.sh [outdir] [scale]` builds the real GUI code (`src/menu.cpp`, `lib/ui`, `lib/gfx`) for the host with stubbed hardware (`tools/screens/stubs/`, `host.cpp`) and saves every screen as PNG plus `sheet.png` (all screens in a grid) and `index.html` (default `build-screens/png/`). The scenarios are in `tools/screens/main.cpp`; add one when you add a screen. Use it to check UI changes and for the manual's pictures.

## Flash layout

| Region | Address | Size |
| --- | --- | --- |
| Bootloader (`bootloader/`, USB-CDC firmware update) | 0x08000000 | 32 KB |
| Application (`STM32G431CBTx_FLASH.ld`, VTOR set in `SystemInit`) | 0x08008000 | 94 KB |
| Settings | last page, 0x0801F800 | 2 KB |

`ENTER_BOOTLOADER` makes the app write a magic word to `TAMP->BKP0R` and reset into the bootloader.
The bootloader shows the update on the OLED (title, hint and progress bar).

## Architecture

**Main loop** (`src/main.cpp`): initializes peripherals, then polls: `tud_task()`, USB→DIN MIDI, `midi::process()`, `display::process()`, `Bluetooth::process()`, `router::process()`, schedulers (10 ms keypress, 30 ms GUI, 50 ms LEDs / 500 ms display refresh), BLE reconnect and status events. Nothing in the loop busy-waits; TinyUSB is only called from the loop, never from interrupts.

**Key modules** (each in `src/` with corresponding header in `inc/`):
- `base` — Clock tree (HSE 48 MHz → PLL → 144 MHz SYSCLK, PLLQ 48 MHz for USB), peripheral clocks, SSR (`base::current`), encoder
- `router` — protocol routing: forwards by destination (Remote→PC over CDC, PC→Remote over BLE), no ACK for forwarded frames, `NAK UNREACHABLE` until the link to the remote is usable (stream open and the peer accepted, see Pairing; BLE down, after a base reset), `NAK BUSY` only on a usable link, ACK + retry queue (3 × 500 ms, same PacketId) for packets the base originates, dedupe, keepalive to the remote every 2 s with `[pcAlive]` (sent at once when the PC state changes; the remote's "Cekam na PC" splash), CDC TX buffer that handles short writes. `GET_INFO` → ACK + `INFO_RESP` (TLV, `lib/protocol` `inc/protocol/info.hpp`, built by `buildInfo()`): git revision/branch/build time/dirty, uptime, reset cause, fault count, BLE flags (connected, stream open, peer accepted, module up, remote alive, PC alive), stored remote MAC, name, pairing state, display, SSR — shown on the PC status page. Observes forwarded `SIGNAL_*` to keep the OLED in sync and shows the progress of a remote firmware update relayed from the PC (`boot::FwProgress`, `GUI::showFwProgress`)
- `bluetooth` — RN4870 or RNBD350 (bench since 2026-11-04) over USART2. The RNBD350 appends a connection handle to status events (`%CONNECT,0,<MAC>,0072%`, `%DISCONNECT,0072%`), so status words are matched with `blestatus::isWord()` and the MAC with `parseConnect()` (`src/logic/ble_status`, host tests `cmake -S . -B build-host -G Ninja -DHOST_TESTS=ON && cmake --build build-host && ctest --test-dir build-host`); `%REBOOT%` wait 3 s. Without `%REBOOT%` after the RST pulse the base configures anyway (a broken RST joint still leaves a working module). **Module self-test** on Nastaveni → Bluetooth, first line (`blestatus::health()`): `Modul: OK`, `OK, bez RST` (RST → NMCLR), `neodpovida` (bytes came, commands fail: MCU TX → module RX), `mlci` (nothing ever received: module TX → MCU RX or power), `start...`. Otherwise: ISR only fills ring buffers; parsing and a non-blocking command-mode state machine (`$$$` … `---`) run in `process()`; TX through a DMA-fed ring buffer. Module config `SN,MIDIControlBase`, `SA,2`, `SR,0000`, `SS,C0`, `R,1`. **`SA,2` (NoInputNoOutput, Just Works) on both base and remote is intentional**: base `SA,2` + remote `SA,1` ended every bond in `%ERR_SEC%` (bench 2026-10-19, DS50002466 Table 2-2); with `SA,1` on the base too the modules show a passkey and still fail. `C` and `F` answer `Trying`/`Scanning` without a `CMD> ` prompt; the engine takes those words as the answer. A running scan (`F`) keeps the RNBD350 in command mode and it answers neither `---` nor `$$$`, so the engine does not exit command mode after `F`; the `X` and `C` queued after "remote found" go out in the same session (bench 2026-11-05).
- **Pairing** (menu Nastaveni → "Sparovat ovladac", `Bluetooth::startPairing()`): `[K,1]`, `Z`, `F`; the main loop (`main.cpp`) connects only to an advertisement named `MIDIControlRemote` (`X`, `C,<type>,<addr>`), sends `B` on `%CONNECT%` without the OLED question and ends on `%SECURED%`/`%BONDED%` (`PairingState::DONE`); `PAIRING_TIMEOUT` 15 s, `%ERR_SEC%`, a disconnect or a module reboot → `FAILED`. Auto-reconnect is off while pairing. At `DONE` the remote's address is stored in the settings page (`settings::remoteAddr`, settings v2; v1 migrates with an empty address, so **after the upgrade from a v1 release the remote must be paired once from the menu**). With no stored remote the base does not auto-reconnect at all (no `C` every 5 s) and the main menu title shows `Sparuj ovladac` until a pairing succeeds (without a PC the "Odpojeno" overlay covers that title, so its comment reads `Sparuj ovladac` instead of `Cekam na PC`). Only the base's own pairing bonds a device: a `%CONNECT%` while `CONNECTING` must come from the address passed to `connectTo()`, otherwise `K,1` and `FAILED`; a `%CONNECT%` while `SCANNING` gets `K,1`; outside pairing a device other than the stored remote gets `K,1` (no `B`, no prompt — the old "Sparovat zarizeni?" dialog is gone). **Protocol frames from BLE are executed only from an accepted peer** (`Bluetooth::_peerAccepted`): set on `%SECURED%`/`%BONDED%` on a link whose `%CONNECT%` address matched the stored remote (or the pairing address while `BONDING`), cleared with the RX packet queue and parser on `%CONNECT%`, `%DISCONNECT%`, `%REBOOT%`, `restart()` and a BLE wedge. Until then received frames are dropped (the remote retries 3 × 500 ms) and nothing is sent to the peer (`canSendData()` false, PC gets `NAK UNREACHABLE`). The address alone is only a claim: a `%SECURED%` without a matching address and a `%ERR_SEC%` outside pairing (a peer without our key) both get `K,1`. After a reconnect to the remote (`C` to the last bond) the link is not secured by itself (DS50002466, `B`): the base sends `B` after `%STREAM_OPEN%` (`B` right after `%CONNECT%` made the module drop the link). Outside pairing a link that stays connected without an accepted peer is not left up (`secureWatchdog()` in `main.cpp`): 3 s after `%STREAM_OPEN%` (or `%CONNECT%`) without `%SECURED%`/`%ERR_SEC%` the stored remote gets `B` once more (also when the first `B` was not queued), 3 s later `K,1` (repeated every 3 s while the link stays up; any other address gets only `K,1`), and the 5 s auto-reconnect retries. The base normally does not advertise (it is the central), so inbound connections need an explicit `A`. **A peer that vanishes (remote module reset/powered off) is reported by the RNBD350 as `%ERR_CONN%`, never `%DISCONNECT%`** (bench 2026-11-07): `main.cpp` treats `ERR_CONN` while connected as link lost (detected after the 2.5 s supervision timeout, `tools/basetest/test_link_loss.py`); while not connected it is only a failed attempt. The command engine keeps commands queued while it waits for `END` (they run in the next `$$$` session). The remote's RN4870 keeps its own bond table; re-pairing after `U,Z` on the base works with `SA,2`
- **Range and link tuning** (2026-11-07, DS50003684B; this module runs command set v1.0, so `GICP`/`QRSS`/`SICP` do not exist: use `GT`/`M`/`ST`): the configuration job also sends `SGA,0F` / `SGC,0F` (maximum TX power while advertising / connected; the module caps it, `MTP` read 12 dBm current / 14 dBm max) and `ST,0010,0010,0000,00FA` (20 ms interval, no latency so notes are not delayed, 2.5 s supervision timeout; the 720 ms module default dropped the link in every fade; rule 5.3.4.3: interval x 1.25 x (1+latency) x 5 < timeout x 10 ms). `ST` alone did NOT take effect on the link made by `C` (the remote's own default won), so after `%SECURED%` on an accepted peer the base sends per connection `CSPHY,<handle>,04,04,02` (Coded PHY S8, about +12 dB link budget, 125 kbit/s raw: fine for MIDI frames, slower for the firmware relay) and `T,0010,0010,0000,00FA`; neither survives a reboot or a new link, the handle is the 4th field of `%CONNECT%`. A short 2M / 720 ms flip-flop right after stream open is the remote module re-proposing its defaults; the base's second request wins (final state Coded, 2500 ms). `%CONN_PARAM%` values are HEX (the old `%d` scan read `0048` as 48). **Link metrics** (`logic/link_stats`, host tests): RSSI by `M` in a `$$$` session every 7 s (not 10: the PC polls the remote diag every 10 s and a coinciding poll gets NAK BUSY) while the link is accepted, the engine idle and the remote quiet for 300 ms (`Bluetooth::_rssiPollDisabled` stops it, the basetest `base` fixture sets it; never during heavy traffic such as a firmware relay; the reply is signed hex, `-2E` = -46 dBm), parameters from `%CONN_PARAM%`, PHY from `%PHY_UPDATED%`, counters (link losses, `%ERR_CONN%`, unexpected `%REBOOT%`, secure retries, last reconnect ms) in `Bluetooth::_stats` (16-bit fields: read them with 16-bit gdb reads, byte reads tear) and in `INFO_RESP` tags 0x14 (RSSI own+peer), 0x15 (interval, timeout, latency, PHY), 0x17 (8-bit counters, reconnect ms), written last because the remote's payload is nearly at the 128-byte limit; the remote's RSSI comes back in `KEEPALIVE_ACK`, the base sends its own in byte 1 of `KEEPALIVE`. An `%REBOOT%` that is not the answer to our `R,1`/RST (module brown-out) is logged as a WARNING and counted. The module's PDS is rewritten by the configuration job on every boot as before (`R,1` follows); `SGA`/`SGC` have no getter, so they cannot be read back. Bench cannot prove range: check it in the church (the INFO signal values show it). **Coded reconnect** (2026-11-07): the remote advertises extended on Coded (see the remote's CLAUDE.md), so the base scans with `FE,01E0,0190,03,00` and connects (reconnect and pairing) with `CE,<type>,<addr>,05,...` for 1M + Coded; `C` cannot reach it. `CE` does not secure the link, `B` follows `%STREAM_OPEN%` as always. A link made together with a new bond over Coded goes silent, so `pairingFinished(true)` sends `K,1` and the auto-reconnect secures it ~5 s later.
- `display` — church indicator over USART1 (1200 Bd, half-duplex, 9-byte frames); one frame per `NOW_PLAYING_INFO`, never aborts a frame in flight; per-digit rendering; SysEx echo of the state to the PC
- `midi` — DIN MIDI (USART3, 31250 Bd): DIN→USB parser (running status, real-time inside SysEx) in the ISR, USB writes from the main loop
- `oled`, `menu` — SH1106 framebuffer (I2C1 400 kHz, DMA) and the menu (`GUI::`). Main menu: Prehraj, Nahraj, Nastaveni; "Napajeni varhan" (SSR checkbox) is in Nastaveni. While the PC player is not connected (`router::isPcAlive()` false) the "Odpojeno" overlay splash ("Cekam na PC") (`noPcScreen`) covers the main menu: `render()` pushes it whenever the main menu is on top without a PC and pops it when it is on top and the PC is back. Enter opens Nastaveni, whose Zpet returns to the overlay. Everything else (pairing status, now playing, remote update) is pushed above it, so it never hides them. Now playing (`NowPlayingScreen`, pushed on `SIGNAL_PLAYING`) has the remote player's layout: state in the title (Prehravam/Zastavuji/Zastaveno, the last for 1.5 s after `SIGNAL_STOPPED`), the code in Font_11x18, the section from `SIGNAL_ANCHOR` centered ("Sloka 2 +1" → "Sloka: 2 (x1)", repeats left), and the Stop icon button (always selected, the only input: encoder press = `STOP_SONG`)
- `led` — WS2812 strips (front 4 px, rear 6 px)
- `usb` — TinyUSB composite device: CDC 0 (interfaces 0/1, EP 0x81/0x02/0x82, protocol frames only), MIDI (2/3, EP 0x03/0x83), CDC 1 "MIDIControl log" (4/5, EP 0x84/0x05/0x85, text log)
- `fault`, `wdg` — fault records (HardFault/MemManage/BusFault/UsageFault/NMI, `signalError(code)`, stmcpp errors, loop stall) in no-init RAM next to the log (`fault::_record`, 92 B, magic + CRC-32), printed in the boot banner once (E) and by `dump`; with a probe attached (DHCSR.C_DEBUGEN) → `bkpt`, otherwise `NVIC_SystemReset()`. Loop watchdog in SysTick (1000 ms without `wdg::kick()` → LOOP_STALL with the interrupted pc) + IWDG ~2 s (LSI /32, RLR 2000), started on every boot (a software IWDG does not survive a reset on the G431). A failed RN4870 init/configure is not fatal (R21): one `E ble:` line + one ERROR record (`fault::recordNonFatal`, no reset, marked announced), the base runs without BLE and retries init every 60 s. Blocking waits (RN4870 init, `Oled::flush`, DIN send) call `wdg::kick()`. `fault::testInject` (1 bus fault, 2 stall, 3 IWDG) is the test hook. **Stuck-note recovery:** after an IWDG/WWDG reset, or a software reset that follows an unannounced fault record (`logic/recovery`, host tests), `main()` sends All Sound Off + All Notes Off (CC 120/123, all 16 channels) on DIN right after `midi::init()` and logs `reset after hang/fault`; the SSR pin (PA4) comes up low from the static GPIO init, so the organ's current source is off after every reset. **Decoding a record:** the banner/`dump` line gives `pc=`, `lr=`, `cfsr=` (the `CFSR` bits are named); resolve with `${CROSS_PREFIX}addr2line -f -C -e build/midicontrol-fw.elf <pc>` (symbol table only in Release, no DWARF; use the ELF of the image that crashed, its revision is in the banner). The IWDG stops while a probe halts the core (`DBG_IWDG_STOP`)
- `debug` — text log `[ssssss.mmm] L tag: text` (`Debug::log(level, tag, fmt, ...)`, main loop only; ISRs use `Debug::logFromIsr`) into a 1536 B ring in no-init RAM (NOINIT block 0x20007800/2K: fault block first (`.noinit.fault`, linker-script order: `KEEP(*(.noinit.fault)) KEEP(*(.noinit.log))`), then the log header + ring at 0x20007860; survives resets), sent by `Debug::process()` to the log CDC while the PC has it open; boot banner with git revision and reset cause (`resetcause`); commands `level e|w|i|d`, `dump`, `help` on the log port. Never into the protocol CDC

**Libraries** (`lib/`, git submodules):
- `protocol` — shared packet codec/parser (`midicontrol-protocol`)
- `bootloader` — shared bootloader engine (`midicontrol-bootloader`), used by `bootloader/` and for the image trailer
- `stmcpp` — C++ STM32 register abstraction (GPIO, scheduler, units, …)
- `gfx`, `ui` — framebuffer graphics and menu/navigation framework
- `tinyusb` — USB stack
- `CMSIS_5`, `cmsis-device-g4` — ARM/STM32G4 headers

## Code Conventions

- **Style**: `.clang-format` enforced — tabs for indentation (width 4), 160-char column limit, braces attach, pointers right-aligned (`int *p`)
- **Namespaces**: Each module uses its own namespace (`base::`, `Bluetooth::`, `display::`, `midi::`, `router::`, `GUI::`, `LED::`)
- **C++ dialect**: C++23, `-fno-exceptions`, `-fno-threadsafe-statics`, `-fno-use-cxa-atexit`; FPU flags `-mfpu=fpv4-sp-d16 -mfloat-abi=hard`
- **Warnings as errors**: `-Werror=switch`, `-Werror=return-type`, `-Werror=parentheses`, `-Wfatal-errors`
- **Language**: Code comments, identifiers and variable names are always English (the UI is Czech; the readme is still Czech)
- **UI language**: all user-visible text (OLED menus, screens, messages, hints) is Czech without diacritics (plain ASCII, the fonts have no Czech glyphs). Names stay English: `MIDIControl` and the splash device type `BASE` (`base::DEVICE_TYPE`). Text longer than its line scrolls (`ui::TextScroll`) instead of being cut off. Protocol, RN4870 and USB strings stay as they are. Keep each string within the display (128 px; 7x10 font = 18 chars, 11x18 font = 11 chars) and its `snprintf` buffer. The `ConfirmScreen`/`NumberScreen`/`back()` defaults live in the `lib/ui` submodule (Czech since `5fb5a2a`: `Ano`/`Ne`, `Zpet`); change them there

## Bench (hardware tests)

- Both boards sit on ST-Links, selected by serial: base `004200443335510C35383531`, remote `37FF70064E59313840161343` (`-c "adapter serial <sn>"` before `-f openocd.cfg`). The ST-Links have no SRST wired, so a probe reset is PIN | SOFTWARE in `resetcause`. `st-info --probe` leaves the cores halted; send `monitor resume` before a gdb `detach`. openocd leaves `DHCSR.C_DEBUGEN` set, which turns a real fault into a `bkpt` halt: the harness writes `0xA05F0000` when it releases the probe.
- Logic analyzer (sigrok `D0..D7` = CH1..CH8): CH1/CH2 base display UART (CH2 = MCU to display), CH3/CH4 base BLE UART (CH4 = MCU to RN4870), CH5/CH6 remote I2C (SCL, SDA), CH7/CH8 remote BLE UART (CH8 = MCU to RN4870). `tools/basetest/la.py` holds the map.
- The Otraki USB-MIDI interface stands in for the organ, but the DIN link was dead in both directions on 2026-10-19 (see `../REVIEW.md`), so DIN checks are open.
- Suites, fixtures and how to run them: `tools/basetest/README.md`; findings and open items: `../REVIEW.md` "Hardware test log 2026-10-19/20".

## Hardware Pin Map (key pins)

- PB0/PB2 — Rotary encoder A/B, PB1 — encoder push
- PA4 — SSR current source enable (organ mains)
- PA0 — RN4870 MODE, PA1 — RN4870 RST (active low)
- PA2/PA3 — USART2 TX/RX to RN4870 (no RTS/CTS wired → module flow control off, `SR,0000`)
- PA9/PA10 — USART1 church display (1200 Bd), PC13 — display sense (EXTI, needs SYSCFG clock)
- PB10/PB11 — USART3 DIN MIDI (31250 Bd)
- PA15/PB7 — I2C1 SCL/SDA, OLED at 0x3C
- PB14 (TIM15), PB5 (TIM17) — WS2812B LED data
- PA11/PA12 — USB; the internal D+ pull-up (DPPU, enabled by `tusb_init()`) stays on: the fixed 1k5 (R33) is not effective on the boards in use (bench 2026-10-19, no enumeration without DPPU)
- USB IDs: app `ABCD:1234`, bootloader `ABCD:1235` (placeholder VID)
- ATtiny1607 (U14) runs its own fixed firmware that sets the MIDI port direction and MIDI OUT supply; it needs no communication, so PA5/PA6/PA7 to it are unused
