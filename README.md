# MIDIcontrol base firmware

Firmware for the MIDIcontrol base station (STM32G431CBTx, Cortex-M4F): USB-MIDI card for the PC player, packet router between
the PC (USB-CDC) and the handheld remote (BLE, Microchip RNBD350), SSR driver for organ power, church indicator
(7-segment display) driver, OLED with an encoder menu, status LEDs. C++23, no exceptions.

The whole system (PC software, base, remote, bootloader, protocol) is specified in [`docs/SYSTEM.md`](docs/SYSTEM.md).
Hardware: [`docs/HARDWARE.md`](docs/HARDWARE.md). Architecture and conventions for developers: [`CLAUDE.md`](CLAUDE.md).

## Documents

| File | Content |
| --- | --- |
| `lib/protocol/PROTOCOL.md` | Packet protocol PC - base - remote (authoritative; shared `midicontrol-protocol` submodule) |
| `lib/protocol/INDICATOR-BUS.md` | Church indicator bus (9-byte frames, 1200 Bd) |
| `docs/display-sysex.md` | Display SysEx messages (`F0 4D 43 ...`), also the anchor format in the song files (Czech) |
| `docs/bench-checklist.md` | Bench checks for a firmware change |
| `tools/basetest/README.md` | Hardware tests (USB-CDC, ST-Links, logic analyzer) |

## Build

```bash
git submodule update --init --recursive
mcu build            # or: cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake && cmake --build build -j
mcu size             # flash / RAM against the budget in .mcu-profile.json (RAM is close to its limit)
```

`inc/git.hpp` (revision, branch) is generated on every build by `cmake/git.cmake`.

## Tests

```bash
cmake -S . -B build-host -G Ninja -DHOST_TESTS=ON
cmake --build build-host && ./build-host/logic_tests    # host unit tests of src/logic
tools/screens/export.sh                                  # every OLED screen as PNG (host build, stubbed hardware)
```

Hardware tests need the bench: see `tools/basetest/README.md`.
