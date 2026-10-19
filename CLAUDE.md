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

Requires `arm-none-eabi-gcc`. A git metadata header (`inc/git.hpp`) is auto-generated on each build via `cmake/git.cmake`. There are no unit tests in this project.

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
- `router` — protocol routing: forwards by destination (Remote→PC over CDC, PC→Remote over BLE), no ACK for forwarded frames, `NAK UNREACHABLE/BUSY` when it cannot forward, ACK + retry queue (3 × 500 ms, same PacketId) for packets the base originates, dedupe, keepalive to the remote every 2 s, CDC TX buffer that handles short writes. Observes forwarded `SIGNAL_*` to keep the OLED in sync
- `bluetooth` — RN4870 over USART2: ISR only fills ring buffers; parsing and a non-blocking command-mode state machine (`$$$` … `---`) run in `process()`; TX through a DMA-fed ring buffer
- `display` — church indicator over USART1 (1200 Bd, half-duplex, 9-byte frames); one frame per `NOW_PLAYING_INFO`, never aborts a frame in flight; per-digit rendering; SysEx echo of the state to the PC
- `midi` — DIN MIDI (USART3, 31250 Bd): DIN→USB parser (running status, real-time inside SysEx) in the ISR, USB writes from the main loop
- `oled`, `menu` — SH1106 framebuffer (I2C1 400 kHz, DMA) and the menu (`GUI::`)
- `led` — WS2812 strips (front 4 px, rear 6 px)
- `usb` — TinyUSB composite device (CDC + MIDI)
- `debug` — log into a RAM ring buffer (never into the CDC stream, which carries only protocol frames)

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
- **UI language**: all user-visible text (OLED menus, screens, messages, hints) is Czech without diacritics (plain ASCII, the fonts have no Czech glyphs). Names stay English: `MIDIControl` and the splash device type `BASE` (`base::DEVICE_TYPE`). Text longer than its line scrolls (`ui::TextScroll`) instead of being cut off. Protocol, RN4870 and USB strings stay as they are. Keep each string within the display (128 px; 7x10 font = 18 chars, 11x18 font = 11 chars) and its `snprintf` buffer. The `ConfirmScreen` "Yes"/"No" labels are hard-coded in the `lib/ui` submodule and must be changed there

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
