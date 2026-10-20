# basetest - hardware tests of the base (STM32G431)

The PC talks to the base over USB-CDC (`abcd:1234`, interface 00, frames only); a second CDC ("MIDIControl log",
interface 04) carries the human-readable log. Two ST-Links read firmware state and inject
actions; the fx2lafw logic analyzer (`la.py`) checks the UARTs. Everything here is opt-in: hardware tests carry the
`hw` marker (`pytest.ini` runs `-m "not hw"` by default).

## Bench

- Base on ST-Link serial `39FF71064B43353943122243` (openocd gdb 3334, tcl 6668, telnet 6667).
- Remote on ST-Link serial `37FF70064E59313840161343` (gdb 3333, tcl 6666, telnet 6665), ON/OFF held.
- Never run `st-info --probe` (resets and halts the core). Before a gdb `detach` send `monitor resume` (dbg.py does).
- Only one openocd per probe; `dbg.Target` starts and stops its own - no other server may be running.
- Logic analyzer: D0/D1 display UART, D2/D3 base BLE UART, D4/D5 remote I2C, D6/D7 remote BLE UART (see `la.py`).

## Setup

```bash
uv venv .venv && uv pip install --python .venv/bin/python pyserial pytest
# flash the base (never the plain .hex; -update.hex or -full.hex)
openocd -c "adapter serial 39FF71064B43353943122243" -f openocd.cfg -c "program build/midicontrol-fw-update.hex verify reset exit"
```

Symbols of globals (`Debug::_log`, ...) are resolved from the ELF symbol table (`nm -C`), so the normal Release
build is enough. Struct fields (`ui::MenuItem` layout, remote `GUI::playerScreen.state_`) need DWARF. Do NOT pass
`-DCMAKE_CXX_FLAGS=-g`: the toolchain file forces `CMAKE_*_FLAGS` into the cache, so that replaces its flags and
changes the generated code. Build a `-g` copy in a SEPARATE dir with a wrapper toolchain that only appends `-g` to the
Release flags (verified: `arm-none-eabi-size` text/data/bss identical to `build/`), and leave `build/` alone:

```bash
cat > /tmp/tc-g.cmake <<EOF
include($PWD/cmake/toolchain-arm-none-eabi.cmake)
set(CMAKE_C_FLAGS_RELEASE "-Os -flto -g" CACHE INTERNAL "")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -flto -g" CACHE INTERNAL "")
set(CMAKE_ASM_FLAGS_RELEASE "-g" CACHE INTERNAL "")
EOF
cmake -S . -B /tmp/build-g -DCMAKE_TOOLCHAIN_FILE=/tmp/tc-g.cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_BOOTLOADER=OFF
cmake --build /tmp/build-g -j
arm-none-eabi-gdb -batch -ex 'print sizeof(ui::MenuItem)' -ex 'print &((ui::MenuItem*)0)->checked_' /tmp/build-g/midicontrol-fw.elf
```

## Run

```bash
cd tools/basetest
../../.venv/bin/python -m pytest -q test_proto_offline.py     # no hardware
../../.venv/bin/python -m pytest -q -m hw                      # with the bench
```

## Modules

- `usblink.Link` - finds the **protocol** port: sysfs `bInterfaceNumber == 00` of `abcd:1234` (by-id `-if00`), never
  the log CDC; creates a missing (or wrong major:minor) `/dev/ttyACMn` with `sudo mknod` in containers without udev. Reader thread -> `FrameParser` -> queue. `recv(timeout, pred)` returns the first
  packet matching `pred`; the others stay for later calls. With `keepalive=True` a background thread sends
  KEEPALIVE to the base every second (the base counts the PC dead after 6 s) and swallows the matching
  KEEPALIVE_ACKs (`keepalive_acks`); KEEPALIVEs from the base are answered. `.stray` must stay empty.
- `dbg.Target("base"|"remote")` - `sym`, `read`, `write`, `u`, `read_u8`, `read_bool`, `cstr`, `core_state`, `gpio`,
  `reset` (reset run), `call("ns::func(args)")` (gdb; the function is found by `nm` and called through a cast
  pointer, returns int; it first stops at a main-loop safe point: base `Bluetooth::process`, remote
  `base::Keyboard::process`), `debug_log` (base); remote: `key`, `type_code`, `goto_player`, `player_state`,
  `player_message`, `screen_name`, `hold_ble_reset(on)` (RN4870 RST = PB8 via GPIOB->BSRR, core running).
  `dbg.probe_holder(name)` names a foreign openocd holding a probe; the `remote_probe` fixture skips then.
- `conftest.wait_until(pred, timeout)`, `conftest.remote_alive(base)` (router::isRemoteAlive computed from
  `router::_lastRemoteRx`/`_remoteEverSeen` and the systick: LTO drops the unused function itself).
- `usblink.Link.detach()` / `attach()` around a base reset (the CDC port disappears and comes back).
- `loglink.LogLink` - opens the log CDC (sysfs `interface` string "MIDIControl log"). Reader thread -> complete lines in
  `.history`; `lines(timeout, pred)`, `wait_for(regex, timeout)` read from a cursor (`skip()` jumps to the end),
  `command("level d")` returns the reply lines (lines without the log prefix). `loglink.is_log_line` = the exact
  format `[ssssss.mmm] L tag: text`; `is_marker` = `--- log before reset ---`, `--- end of log before reset ---`,
  `[dropped N]`.
- `la.uart_timed` - like `la.uart` but returns `[(time, byte)]` plus decoder warnings (frame errors).
- `proto.py` - copy of the remote harness codec.

## Remote symbols (DWARF)

The remote's Release ELF has no DWARF; `key()` and the screen fields need it. Build a `-g` copy with identical code in a
separate dir (only append `-g` to the Release flags; the build regenerates the remote's `inc/git.hpp`, restore it) and
point `BASETEST_REMOTE_ELF` at it:

```bash
S=/path/to/scratch
printf '%s\n' 'include(/workspace/MIDICONTROL/midicontroller-fw/cmake/toolchain-arm-none-eabi.cmake)' \
  'set(CMAKE_C_FLAGS_RELEASE "-O2 -g" CACHE INTERNAL "")' 'set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g" CACHE INTERNAL "")' \
  'set(CMAKE_ASM_FLAGS_RELEASE "-g" CACHE INTERNAL "")' > $S/tc-g.cmake
(cd ../../../midicontroller-fw && cmake -S . -B $S/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$S/tc-g.cmake \
  && cmake --build $S/build; git checkout inc/git.hpp)
# check that .text equals the flashed build: arm-none-eabi-objcopy -O binary -j .text <elf> <out>; cmp
export BASETEST_REMOTE_ELF=$S/build/midicontroller-fw.elf
```

## Pairing and relay (`test_pairing.py`, `test_relay.py`)

- Needs the remote powered and advertising, both RN4870 with `SA,2`. `test_pairing.py` clears the base's bonds
  (`U,Z`), pairs, resets the base, holds the remote's RN4870 in reset for the timeout test and re-pairs at the end.
  `PAIRING_LOG` (default `/tmp/basetest-pairing.log`) collects the status lines of both modules.
- `test_relay.py` expects a paired, connected remote and its probe free (`remote_probe`).
- LA: remote BLE D7 = MCU -> RN4870, D6 = RN4870 -> MCU (the module's `%CONNECT%` etc. are on D6).

## Things to know

- Log (`Debug`, `test_log.py`): ring of 1536 B in no-init RAM at 0x20007860 (after the 0x60 B fault block at 0x20007800; order pinned in the linker script) (`Debug::_log`, header `{magic 0x4C4F4721,
  head, tail, dropped, crc32}`, head/tail absolute positions). It survives resets (CCM SRAM, option bit
  CCMSRAM_RST = 1 on the bench base), not power loss. `debug_log()` reads it through the probe (whole lines, oldest
  first). On boot the kept history is sent first between `--- log before reset ---` / `--- end of log before reset ---`.
  Lines are written to the log CDC only while the port is open (DTR) and 200 ms after it opened (the PC flushes its
  input on open); lines overwritten before they were sent are reported as `[dropped N]`. Commands: `level e|w|i|d`
  (default i after every reset), `dump`, `help`, anything else `?`.
- Both openocd's `reset run` (reset_config none = SYSRESETREQ) and `NVIC_SystemReset` set `PINRSTF|SFTRSTF` on the
  G431 (NRST is driven by internal resets), so the banner shows `reset: SOFTWARE|PIN` for both.
- `dbg.sym` also finds statics that LTO renamed to `<mangled>.lto_priv.N` (e.g. `GUI::mainItems`).
- KEEPALIVE_ACK carries the base's own pid, so ACKs are matched FIFO against every KEEPALIVE the Link sent
  (background, `send`, and keepalives inside `send_raw`); only background ones are swallowed, expiry 1 s.
- Base-originated KEEPALIVEs are auto-answered and also delivered to `recv(pred)`.
- `pause_keepalive()` stops background keepalives and auto-answers (the PC goes silent); `pause_keepalive(False)` resumes.
- Thread failures are stored in `Link.error`; `recv`/`send` re-raise, the `link` fixture teardown asserts it is None.

## Fault tests (`test_fault.py`)

- Faults are injected through `fault::testInject` (u8, read once per main-loop pass): `1` = read from 0xFFFFFFF0
  (BusFault, `pc` in `main`), `2` = `fault::testSpin()` with interrupts on (LOOP_STALL after 1000 ms), `3` =
  `fault::testSpinNoIrq()` (IWDG reset after ~2 s, no record).
- **C_DEBUGEN.** With DHCSR.C_DEBUGEN set the firmware stops at `bkpt` in `fault::finish()` instead of resetting.
  The ST-Link sets it on connect, a system reset keeps it, and **exiting openocd does not clear it** (bench
  2026-10-20: after a plain openocd exit an injected stall left the core halted, DHCSR 0x00030003). Only a POR or a
  DHCSR write clears it. `inject(release=True)` / `dbg.Target.release_debug()` therefore writes
  `DHCSR = 0xA05F0000` (DBGKEY, C_DEBUGEN = 0, core keeps running) and then stops openocd at once. The next
  `tcl()` starts openocd again, which sets C_DEBUGEN again.
- While the core is halted (bkpt test) USB is not serviced: detach `link` and close the log first, or the
  keepalive write times out and the session `link` fixture fails.
- openocd's `stm32g4x.cfg` sets `DBG_IWDG_STOP` on examine, and the firmware sets it in `wdg::start()`, so the IWDG
  does not run while the core is halted. The loop watchdog stops too, because SysTick does not tick in debug halt.
- A software-started IWDG does not survive a system reset or its own IWDG reset on the G431 (IWDG_SW = 1). Every
  boot starts it again (`wdg::start()`).
- `dbg.Target.fault_record()` decodes `fault::_record` (92 B, magic 0x46415554, zlib CRC-32 of the first 88 B);
  `addr2line(addr)` gives the function name (symbol table only, Release ELF has no DWARF).
- Relay and remote-key tests need `BASETEST_REMOTE_ELF` (see "Remote symbols"). Without it, gdb key injection fails.

## Remote diagnostics, slots and watchdogs (`test_remote_diag.py`)

- **A remote reset powers the remote off** (it releases PA1). Tests that really trigger a remote HardFault, loop
  stall or IWDG reset are marked `needs_user` and are skipped unless selected with `-m needs_user`; someone at the
  bench must switch the remote on again (the test waits up to 3 min): `test_remote_fault_survives_power_off`,
  `test_remote_loop_stall`, `test_remote_iwdg`. Injection: `fault::testInject` on the remote (`1` unaligned `ldr` →
  HardFault, `2` `testSpin` → LOOP_STALL after 2000 ms, `3` `testSpinNoIrq` → IWDG after ~4 s).
- The M0+ cannot read DHCSR.C_DEBUGEN, so the remote always resets after a fault; set `fault::haltOnFault = 1`
  through the probe to stop at `bkpt` instead (the slot is written first).
- `test_persisted_slot_roundtrip` runs without a reset: gdb calls `diag::testPersist(marker)` (the real persist path:
  interrupts off, polling SPI, one W25Q32 page program), reads the slot back into `diag::testBuf` with
  `diag::testReadSlot`, decodes it in Python (magic 0x544C5344, zlib CRC-32 of bytes 0..239), loads it into the
  ring with `diag::testLoadSlot` and checks that DIAG_GET returns its records and that a cursor past them programs
  the slot's "fetched" byte. Each run uses one of the 16 slots; a boot compacts a full sector.
- `test_watchdogs_do_not_trip`: 10 min (`BASETEST_WDG_SECONDS` overrides) of DIAG polls, relayed signals and key
  injection plus one full-size OTA staging erase (`FW_UPDATE_START`, then `FW_UPDATE_END` without data = BAD_SIZE);
  the remote's boot_id must not change. It also reads `wdg::maxGapMs` (longest gap between kicks, must be < 2000 ms)
  and `wdg::bootToLoopMs`.

## OTA tests (`test_ota.py`)

`test_remote_ota_via_base` performs a **real OTA of the remote** (about 45 s transfer plus a remote reboot; the remote must
be back within 30 s, otherwise the test fails with "BLOCKED" and must not be retried blindly). `test_base_ota_over_cdc` and
`test_base_ota_power_loss` update the base over USB (the power-loss test resets the base through the probe at mid-transfer;
a teardown fixture flashes the base back via the updater if it is left in the bootloader `abcd:1235`).

The PC updater is `midicontrol --cmd "update <base|remote> <file>"` from `../midicontrolpy-sw`. On the bench it is built
against the RtMidi shim (not committed; it provides RtMidiOut/RtMidiIn over ALSA seq, see the Task 6E-3 report):

```bash
SHIM=<dir with the shim: RtMidi.h, lib/librtmidi.so, lib/rtmidi.pc>  SCRATCH=<any scratch dir>
PKG_CONFIG_PATH=$SHIM/lib cmake -S ../midicontrolpy-sw -B $SCRATCH/pc6e3/b && cmake --build $SCRATCH/pc6e3/b -j
```

Environment:

| Variable | Meaning |
|---|---|
| `BASETEST_UPDATER` | **required**: path of the shim-built `midicontrol` binary |
| `BASETEST_REMOTE_BIN` | remote image, default `../midicontroller-fw/build/midicontroller-fw-update.bin` |
| `BASETEST_BASE_BIN` | base image, default `build/midicontrol-fw-update.bin` (build it first; the test compares the banner revision with `inc/git.hpp`) |
| `BASETEST_OUT` | directory for the updater logs `updater-*.log` (default: a temp dir per run) |

The updater runs with its own config (WEB off, no log port) and XDG dirs; the harness link is detached while it runs, the
log CDC stays with the harness.

## Soak

`SOAK_SECONDS=600 BASETEST_REMOTE_ELF=<-g elf> ../../.venv/bin/python -m pytest -q -m hw -s test_soak.py` - 3 s cycles
(PLAY from the remote by key injection, SIGNAL_PLAYING, 3x SIGNAL_ANCHOR, NOW_PLAYING_INFO, USB-MIDI notes,
SIGNAL_STOPPED) plus DIAG_GET every 10 s with DIAG_GET -> DIAG_RESP latency. Per-cycle counters go to `$SOAK_OUT/soak.csv`,
the summary to `$SOAK_OUT/summary.txt`. DIN is not checked (bench link broken). At the default INFO level the base logs nothing in steady state, so
"log clean" means no E/W lines.
One lost DIAG poll fails the soak by design. Base BLE-side CRC errors are NOT measured (no counter in bluetooth.cpp).
CDC bad frames: `router::_badFrames` is zeroed after each report, so they show as `cdc: N bad frames dropped` W lines;
that count covers all bad CDC frames (CRC, length, incomplete), not only CRC. Remote CRC_ERRORS diag records are counted.
The log CDC is positive-checked at start (`level d`, then back to the default INFO). The age of the last remote packet is
sampled >= 4 Hz (`age_samples`) and must stay below 3000 ms.
