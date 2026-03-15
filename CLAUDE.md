# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

STM32G431CBTx (Cortex-M4, 144 MHz) firmware for a MIDI controller with Bluetooth (RN4020), OLED display (SH1106 via I2C), USB MIDI (TinyUSB), RGB LEDs, and an external 7-segment display. Written in C++23 with no exceptions, no heap allocation in UI/graphics code.

## Build Commands

```bash
# Configure (from project root)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake

# Build
cmake --build build

# Flash via OpenOCD + ST-Link
openocd -f openocd.cfg -c "program build/midicontrol-fw.elf verify reset exit"
```

Requires `arm-none-eabi-gcc` toolchain. Build produces `.elf`, `.hex`, and `.bin` in the build directory. A git metadata header (`inc/git.hpp`) is auto-generated on each build via `cmake/git.cmake`.

There are no unit tests in this project.

## Architecture

**Main loop** (`src/main.cpp`): Initializes peripherals and runs a scheduler-driven event loop. Schedulers fire at fixed intervals (e.g., 10ms for keypress polling, 500ms for LED updates, ~30ms for GUI rendering).

**Key modules** (each in `src/` with corresponding header in `inc/`):
- `base` — Clock tree setup (HSE 8MHz → PLL → 144MHz), GPIO init (encoder, power control)
- `bluetooth` — RN4020 BLE module over USART2, command/data mode switching, connection state machine
- `oled` — SH1106 OLED framebuffer rendering via I2C, auto-sleep
- `display` — External 7-segment display protocol over USART (9-byte frames driven by SysEx MIDI)
- `midi` — USB MIDI and BLE MIDI packet handling
- `led` — RGB LED strips (front 4px, rear 6px) with HSV support
- `usb` — TinyUSB device stack setup (MIDI + CDC classes)

**Libraries** (`lib/`, all git submodules):
- `stmcpp` — C++ STM32 HAL abstraction (GPIO, USART, I2C, DMA, timers, scheduler)
- `gfx` — Hardware-independent framebuffer graphics (fonts, icons, shapes)
- `ui` — Menu/navigation framework (Navigator with screen stack: MenuScreen, SplashScreen, ParagraphScreen, NumberScreen, ConfirmScreen)
- `tinyusb` — USB stack
- `CMSIS` — ARM/STM32G4 device headers (not a submodule)

## Code Conventions

- **Style**: `.clang-format` enforced — tabs for indentation (width 4), 160-char column limit, braces attach, pointers right-aligned (`int *p`)
- **Namespaces**: Each module uses its own namespace (`base::`, `Bluetooth::`, `display::`, `GUI::`, `LED::`)
- **C++ dialect**: C++23, `-fno-exceptions`, `-fno-threadsafe-statics`, `-fno-use-cxa-atexit`
- **Warnings as errors**: `-Werror=switch`, `-Werror=return-type`, `-Werror=parentheses`, `-Wfatal-errors`
- **Language**: Comments and readme are in Czech

## Hardware Pin Map (key pins)

- PA0/PA1/PA2 — Rotary encoder (up/down/push)
- PA4 — Power enable (current control)
- PC13 — Display sense (external display detection)
- USART1 — Debug interface
- USART2 — Bluetooth (RN4020)
- I2C — OLED (addr 0x3C)
- USB — TinyUSB device (VID:0xCAFE, PID:0x4000)
