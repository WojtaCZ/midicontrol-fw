# basetest - hardware tests of the base (STM32G431)

The PC talks to the base over USB-CDC (`abcd:1234`, frames only). Two ST-Links read firmware state and inject
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

Symbols of globals (`Debug::_logRing`, ...) are resolved from the ELF symbol table (`nm -C`), so the normal Release
build is enough. Struct fields (`GUI::playerScreen.state_`, used on the remote) need DWARF: build that firmware with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g -DCMAKE_ASM_FLAGS=-g`.

## Run

```bash
cd tools/basetest
../../.venv/bin/python -m pytest -q test_proto_offline.py     # no hardware
../../.venv/bin/python -m pytest -q -m hw                      # with the bench
```

## Modules

- `usblink.Link` - finds the port (by-id, else sysfs VID:PID; creates a missing `/dev/ttyACMn` with `sudo mknod`
  in containers without udev). Reader thread -> `FrameParser` -> queue. `recv(timeout, pred)` returns the first
  packet matching `pred`; the others stay for later calls. With `keepalive=True` a background thread sends
  KEEPALIVE to the base every second (the base counts the PC dead after 6 s) and swallows the matching
  KEEPALIVE_ACKs (`keepalive_acks`); KEEPALIVEs from the base are answered. `.stray` must stay empty.
- `dbg.Target("base"|"remote")` - `sym`, `read`, `u`, `core_state`, `gpio`, `key` (remote), `debug_log` (base).
- `proto.py` - copy of the remote harness codec.

## Things to know

- `debug_log()` reads `Debug::_logRing` (512 B ring, `_logHead`); the ring is `[[gnu::used]] volatile` so LTO keeps it.
  After a wrap the first line may be cut.
- KEEPALIVE_ACK carries the base's own pid, so ACKs are matched FIFO against every KEEPALIVE the Link sent
  (background, `send`, and keepalives inside `send_raw`); only background ones are swallowed, expiry 1 s.
- Base-originated KEEPALIVEs are auto-answered and also delivered to `recv(pred)`.
- `pause_keepalive()` stops background keepalives and auto-answers (the PC goes silent); `pause_keepalive(False)` resumes.
- Thread failures are stored in `Link.error`; `recv`/`send` re-raise, the `link` fixture teardown asserts it is None.
