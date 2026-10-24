# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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

## Flash layout

| Region | Address | Size |
| --- | --- | --- |
| Bootloader (`bootloader/`, USB-CDC firmware update) | 0x08000000 | 32 KB |
| Application (`STM32G431CBTx_FLASH.ld`, VTOR set in `SystemInit`) | 0x08008000 | 94 KB |
| Settings | last page, 0x0801F800 | 2 KB |

`ENTER_BOOTLOADER` makes the app write a magic word to `TAMP->BKP0R` and reset into the bootloader.

## Architecture

**Main loop** (`src/main.cpp`): initializes peripherals, then polls: `tud_task()`, USB→DIN MIDI, `midi::process()`, `display::process()`, `Bluetooth::process()`, `router::process()`, schedulers (10 ms keypress, 30 ms GUI, 50 ms LEDs / 500 ms display refresh), BLE reconnect and status events. Nothing in the loop busy-waits; TinyUSB is only called from the loop, never from interrupts.

**Key modules** (each in `src/` with corresponding header in `inc/`):
- `base` — Clock tree (HSE 48 MHz → PLL → 144 MHz SYSCLK, PLLQ 48 MHz for USB), peripheral clocks, SSR (`base::current`), encoder
- `router` — protocol routing: forwards by destination (Remote→PC over CDC, PC→Remote over BLE), no ACK for forwarded frames, `NAK UNREACHABLE` until the link to the remote is usable (stream open and the peer accepted, see Pairing; BLE down, after a base reset), `NAK BUSY` only on a usable link, ACK + retry queue (3 × 500 ms, same PacketId) for packets the base originates, dedupe, keepalive to the remote every 2 s, CDC TX buffer that handles short writes. Observes forwarded `SIGNAL_*` to keep the OLED in sync
- `bluetooth` — RN4870 over USART2: ISR only fills ring buffers; parsing and a non-blocking command-mode state machine (`$$$` … `---`) run in `process()`; TX through a DMA-fed ring buffer. Module config `SN,MIDIControlBase`, `SA,2`, `SR,0000`, `SS,C0`, `R,1`. **`SA,2` (NoInputNoOutput, Just Works) on both base and remote is intentional**: base `SA,2` + remote `SA,1` ended every bond in `%ERR_SEC%` (bench 2026-10-19, DS50002466 Table 2-2); with `SA,1` on the base too the modules show a passkey and still fail. `C` and `F` answer `Trying`/`Scanning` without a `CMD> ` prompt; the engine takes those words as the answer.
- **Pairing** (menu Nastaveni → "Sparovat ovladac", `Bluetooth::startPairing()`): `[K,1]`, `Z`, `F`; the main loop (`main.cpp`) connects only to an advertisement named `MIDIControlRemote` (`X`, `C,<type>,<addr>`), sends `B` on `%CONNECT%` without the OLED question and ends on `%SECURED%`/`%BONDED%` (`PairingState::DONE`); `PAIRING_TIMEOUT` 15 s, `%ERR_SEC%`, a disconnect or a module reboot → `FAILED`. Auto-reconnect is off while pairing. At `DONE` the remote's address is stored in the settings page (`settings::remoteAddr`, settings v2; v1 migrates with an empty address, so **after the upgrade from a v1 release the remote must be paired once from the menu**). With no stored remote the base does not auto-reconnect at all (no `C` every 5 s) and the main menu title shows `Sparuj ovladac` until a pairing succeeds. Only the base's own pairing bonds a device: a `%CONNECT%` while `CONNECTING` must come from the address passed to `connectTo()`, otherwise `K,1` and `FAILED`; a `%CONNECT%` while `SCANNING` gets `K,1`; outside pairing a device other than the stored remote gets `K,1` (no `B`, no prompt — the old "Sparovat zarizeni?" dialog is gone). **Protocol frames from BLE are executed only from an accepted peer** (`Bluetooth::_peerAccepted`): set on `%SECURED%`/`%BONDED%` on a link whose `%CONNECT%` address matched the stored remote (or the pairing address while `BONDING`), cleared with the RX packet queue and parser on `%CONNECT%`, `%DISCONNECT%`, `%REBOOT%`, `restart()` and a BLE wedge. Until then received frames are dropped (the remote retries 3 × 500 ms) and nothing is sent to the peer (`canSendData()` false, PC gets `NAK UNREACHABLE`). The address alone is only a claim: a `%SECURED%` without a matching address and a `%ERR_SEC%` outside pairing (a peer without our key) both get `K,1`. After a reconnect to the remote (`C` to the last bond) the link is not secured by itself (DS50002466, `B`): the base sends `B` after `%STREAM_OPEN%` (`B` right after `%CONNECT%` made the module drop the link). Outside pairing a link that stays connected without an accepted peer is not left up (`secureWatchdog()` in `main.cpp`): 3 s after `%STREAM_OPEN%` (or `%CONNECT%`) without `%SECURED%`/`%ERR_SEC%` the stored remote gets `B` once more (also when the first `B` was not queued), 3 s later `K,1` (repeated every 3 s while the link stays up; any other address gets only `K,1`), and the 5 s auto-reconnect retries. The base normally does not advertise (it is the central), so inbound connections need an explicit `A`. The command engine keeps commands queued while it waits for `END` (they run in the next `$$$` session). The remote's RN4870 keeps its own bond table; re-pairing after `U,Z` on the base works with `SA,2`
- `display` — church indicator over USART1 (1200 Bd, half-duplex, 9-byte frames); one frame per `NOW_PLAYING_INFO`, never aborts a frame in flight; per-digit rendering; SysEx echo of the state to the PC
- `midi` — DIN MIDI (USART3, 31250 Bd): DIN→USB parser (running status, real-time inside SysEx) in the ISR, USB writes from the main loop
- `oled`, `menu` — SH1106 framebuffer (I2C1 400 kHz, DMA) and the menu (`GUI::`)
- `led` — WS2812 strips (front 4 px, rear 6 px)
- `usb` — TinyUSB composite device: CDC 0 (interfaces 0/1, EP 0x81/0x02/0x82, protocol frames only), MIDI (2/3, EP 0x03/0x83), CDC 1 "MIDIControl log" (4/5, EP 0x84/0x05/0x85, text log)
- `fault`, `wdg` — fault records (HardFault/MemManage/BusFault/UsageFault/NMI, `signalError(code)`, stmcpp errors, loop stall) in no-init RAM next to the log (`fault::_record`, 92 B, magic + CRC-32), printed in the boot banner once (E) and by `dump`; with a probe attached (DHCSR.C_DEBUGEN) → `bkpt`, otherwise `NVIC_SystemReset()`. Loop watchdog in SysTick (1000 ms without `wdg::kick()` → LOOP_STALL with the interrupted pc) + IWDG ~2 s (LSI /32, RLR 2000), started on every boot (a software IWDG does not survive a reset on the G431). A failed RN4870 init/configure is not fatal (R21): one `E ble:` line + one ERROR record (`fault::recordNonFatal`, no reset, marked announced), the base runs without BLE and retries init every 60 s. Blocking waits (RN4870 init, `Oled::flush`, DIN send) call `wdg::kick()`. `fault::testInject` (1 bus fault, 2 stall, 3 IWDG) is the test hook. **Decoding a record:** the banner/`dump` line gives `pc=`, `lr=`, `cfsr=` (the `CFSR` bits are named); resolve with `${CROSS_PREFIX}addr2line -f -C -e build/midicontrol-fw.elf <pc>` (symbol table only in Release, no DWARF; use the ELF of the image that crashed, its revision is in the banner). The IWDG stops while a probe halts the core (`DBG_IWDG_STOP`)
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
- **Language**: Comments and readme are in Czech
- **UI language**: all user-visible text (OLED menus, screens, messages, hints) is Czech without diacritics (plain ASCII, the fonts have no Czech glyphs). Names stay English: `MIDIControl` and the splash device type `BASE` (`base::DEVICE_TYPE`). Text longer than its line scrolls (`ui::TextScroll`) instead of being cut off. Protocol, RN4870 and USB strings stay as they are. Keep each string within the display (128 px; 7x10 font = 18 chars, 11x18 font = 11 chars) and its `snprintf` buffer. The `ConfirmScreen`/`NumberScreen`/`back()` defaults live in the `lib/ui` submodule (Czech since `5fb5a2a`: `Ano`/`Ne`, `Zpet`); change them there

## Bench (hardware tests)

- Both boards sit on ST-Links, selected by serial: base `39FF71064B43353943122243`, remote `37FF70064E59313840161343` (`-c "adapter serial <sn>"` before `-f openocd.cfg`). The ST-Links have no SRST wired, so a probe reset is PIN | SOFTWARE in `resetcause`. `st-info --probe` leaves the cores halted; send `monitor resume` before a gdb `detach`. openocd leaves `DHCSR.C_DEBUGEN` set, which turns a real fault into a `bkpt` halt: the harness writes `0xA05F0000` when it releases the probe.
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
