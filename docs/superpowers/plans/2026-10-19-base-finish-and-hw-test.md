# Base station: finish remaining work and hardware test — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the base (STM32G431, `midicontrol-fw` @ HEAD) up on the bench, close its open work items (first-time pairing with the remote, RN4870 auth unification, remaining review bugs found on hardware), and verify it end to end with the same depth as the remote got on 2026-10-18: protocol conformance over USB-CDC, relay to and from the real remote over BLE, USB-MIDI ↔ DIN through the Otraki interface, church display frames, OTA, link lifecycle and a soak.

**Architecture:** A pytest harness in `tools/basetest/` plays the PC on the base's USB-CDC port (pyserial) and reuses the protocol codec from the remote's harness. A debug helper drives both ST-Links (selected by serial) through openocd + gdb to inject key presses and read state on base and remote while they run. The logic analyzer (fx2lafw, `sigrok-cli`) decodes the display UART (1200 Bd) and both BLE UARTs (115200 Bd). ALSA (`amidi`) drives the base's USB-MIDI port and the Otraki interface wired to the base's DIN IN/OUT. The final check runs the real PC player (`midicontrolpy-sw`) instead of the harness.

**Tech Stack:** Python 3 + pytest + pyserial (venv), openocd + arm-none-eabi-gdb, sigrok-cli, alsa-utils (`amidi`), C++23 firmware (CMake, arm-none-eabi-gcc).

**Spec:** `lib/protocol/PROTOCOL.md` (protocol v2), `CLAUDE.md` (pin map, architecture), `../REVIEW.md` (open items, known bugs, bench), `../midicontroller-fw/tools/bletest/` (remote harness this mirrors).

## Global Constraints

- Probes by serial, always `-c "adapter serial <sn>"` before `-f openocd.cfg`: **base** ST-Link `39FF71064B43353943122243` (gdb port 3334), **remote** ST-Link `37FF70064E59313840161343` (gdb port 3333). Never run `st-info --probe` (it resets and leaves the core halted). Before a gdb `detach`, send `monitor resume`.
- Logic analyzer (fx2lafw, 8 ch, sigrok D0..D7 = CH1..CH8): **D0/D1 base display UART** (1200 Bd), **D2/D3 base BLE UART** (115200 Bd), **D4/D5 remote I2C**, **D6/D7 remote BLE UART** (115200 Bd). TX/RX order within each pair is checked empirically in Task 1 and written into `la.py`.
- Addresses: PC `0x01`, Base `0x02`, Remote `0x03`, Broadcast `0xFF`. CRC is the firmware table (poly 0x31, MSB first, init 0, check value `0xA2` for `"123456789"`).
- USB IDs: app `ABCD:1234` (CDC + MIDI), bootloader `ABCD:1235`.
- Remote: name `MIDIControlRemote`, BLE address `D8:80:39:FE:59:96`. Base RN4870 name `MIDIControlBase`.
- Flash the base with `build/midicontrol-fw-full.hex` (bootloader + app with trailer) or `-update.hex`, never the plain `.hex`.
- UI text Czech without diacritics, fits the display (7x10 font: 18 chars). Comments in Czech. `.clang-format` style (tabs).
- Do not edit `bootloader/`, `lib/bootloader/`, `lib/protocol/` from the firmware repos; protocol fixes go to `midicontrol-protocol` and are bumped.
- Firmware bugs found by tests go through superpowers:systematic-debugging, one change at a time, each with its own commit in the repo it belongs to. Never hold a core halted longer than a test step needs (retry and keepalive timers keep running).
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Do not commit `inc/git.hpp`.
- Diagnostics (Tasks 6A–6E, spec `docs/superpowers/specs/2026-10-19-diagnostics-logging-watchdog-design.md`): protocol CDC (interface 00) carries only protocol frames; base text goes only to the log CDC ("MIDIControl log"). New MsgTypes `DIAG_GET = 0x80`, `DIAG_RESP = 0x81`; record 12 B LE `seq u16, code u8, level u8, uptime_ms u32, arg u32`, ≤ 8 per response. Base no-init block `0x20007800`, 2 KB, in both app and bootloader linker scripts; `sizeBudget.ram` 29184. IWDG base ~2 s, remote ~4 s; loop watchdog 1000 ms base / 2000 ms remote. Log line `[%6lu.%03lu] %c %s: %s\r\n`. Bootloader edits allowed only for the IWDG kick and the base's no-init reservation (exception to the rule above). Never `printf` from an ISR; the log path never blocks the main loop. Flashing the remote needs the user's go-ahead.

## Review Focus

1. **Short writes and frames split across USB packets** (CDC delivers 64-byte packets; the PC may write 1 byte at a time). Parser must reassemble; replies must not interleave. Pinned in Task 3 (`test_split_frame_bytewise`, `test_burst_8_frames`).
2. **Remote unreachable while the PC sends to it** (BLE down). The base must answer `NAK UNREACHABLE (0x06)` at once and not queue stale packets that fire on reconnect. Pinned in Task 5 (`test_pc_to_remote_unreachable`).
3. **USB-MIDI traffic during BLE reconnect attempts and display frames** (the old code busy-waited ~1 s). Notes must keep timing. Pinned in Task 6 (`test_midi_latency_during_reconnect`).
4. **Display updates faster than a 75 ms frame** (several `NOW_PLAYING_INFO` in a row). Each frame on the wire must be complete; the last state must win. Pinned in Task 4 (`test_display_burst_last_wins`).
5. **Pairing with the remote when other BLE devices advertise nearby** (phones, the PC). The base must connect only to `MIDIControlRemote` and give up cleanly after a timeout. Pinned in Task 5 (`test_pairing_timeout_no_remote`) and the manual scan capture in Task 5 Step 1.

6. **Log CDC never opened by a host for hours** (PC software down). The ring wraps with a `[dropped N]` marker; the main loop never blocks on the log. Pinned in Task 6B (`test_ring_wraps_without_host`).
7. **No-init RAM holding garbage** (first power-on, brown-out). Header CRCs reject it; no bogus fault is shown. Pinned in Task 6C (`test_no_bogus_fault_after_garbage`).
8. **Watchdog during long legitimate operations** (BLE reconfigure, OLED flush, DIN bursts, firmware update in the bootloader, the remote's W25Q32 erase). No spurious reset. Pinned in Task 6C (`test_long_operations_do_not_trip`) and Task 7 OTA tests run with the IWDG on.
9. **Remote diagnostics competing with real BLE traffic.** DIAG polling stays ≤ 1 frame per 10 s each way and stops during OTA. Pinned in Task 6D (`test_diag_traffic_budget`) and Task 6E (comm: no DIAG_GET while `FW_UPDATE_*` runs).
10. **Log port disappearing mid-line** (base reset, USB replug) while the PC reads it. No duplicates, no crash, reconnect. Pinned in Task 6E (`test_log_reader_reconnect`).

---

### Task 1: Base on the bench (flash, boot, bus captures)

**Files:**
- Create: `tools/basetest/la.py` (sigrok capture + decode helpers, channel map)
- Modify: none in firmware unless boot fails (then systematic-debugging)

**Interfaces (produced):**
- `la.capture(seconds: float, channels: str, samplerate="1M") -> Path` (`.sr` file in the scratchpad; runs `sigrok-cli -d fx2lafw --channels ... --config samplerate=... --time ...`)
- `la.uart(sr: Path, rx: str, baud: int) -> bytes` (`sigrok-cli -i <sr> -P uart:rx=<ch>:baudrate=<baud> -A uart=rx-data`, parsed into bytes)
- `la.CH = {"disp_tx": "D0", "disp_rx": "D1", "base_ble_tx": "D2", "base_ble_rx": "D3", "rem_scl": "D4", "rem_sda": "D5", "rem_ble_tx": "D6", "rem_ble_rx": "D7"}` (direction fixed after Step 4)

- [ ] **Step 1:** `mcu build` in `midicontrol-fw`. Expected: app, bootloader and `midicontrol-fw-full.hex` link; size within budget (94 KB flash, 30 KB RAM).
- [ ] **Step 2:** Back up the current base flash before overwriting: `openocd -c "adapter serial 39FF71064B43353943122243" -f openocd.cfg -c "init; halt; flash read_bank 0 ../backups/base-g431-flash-2026-10-19.bin; exit"`. Expected: 128 KB file.
- [ ] **Step 3:** Flash: `openocd -c "adapter serial 39FF71064B43353943122243" -f openocd.cfg -c "program build/midicontrol-fw-full.hex verify reset exit"`. Expected: `** Verified OK **`.
- [ ] **Step 4:** Start a 6 s capture of D0–D3 at 1 MHz, then reset the base (`init; reset run; exit`). Expected on the BLE pair: one direction shows `%REBOOT%`, the other `$$$`, `SN,MIDIControlBase`, `SA,2`, `SR,0000`, `SS,C0`, `R,1`, each answered `AOK`, then a second `%REBOOT%`. Fix the TX/RX direction in `la.CH` from this. Within 1 s the display pair shows a 9-byte frame at 1200 Bd.
- [ ] **Step 5:** `lsusb` shows `abcd:1234`, `/dev/ttyACM*` appears, `amidi -l` lists the base's MIDI port next to the Otraki. Through gdb on the base: `pc` in the app (0x0800_8000..0x0801_F800), DHCSR not locked up, `Debug` ring buffer has no errors.
- [ ] **Step 6:** Commit `tools/basetest/la.py`.

### Task 2: Harness — CDC link, debug helper, offline codec

**Files:**
- Create: `tools/basetest/proto.py` (copy of `../midicontroller-fw/tools/bletest/proto.py`, header comment naming the source; repos are independent)
- Create: `tools/basetest/usblink.py`, `tools/basetest/dbg.py`, `tools/basetest/conftest.py`, `tools/basetest/pytest.ini`, `tools/basetest/test_proto_offline.py`, `tools/basetest/README.md`

**Interfaces:**
- Consumes: `proto.encode(src, dst, pid, mtype, payload=b"") -> bytes`, `proto.FrameParser().feed(bytes) -> list[Packet]` (`.crc_errors`, `.overflows`), `proto.MT`, `proto.NAK`, `proto.ADDR_PC/BASE/REMOTE/BCAST` (from the remote harness)
- Produces:
  - `usblink.Link(port=None)` (auto-finds `/dev/serial/by-id/*ABCD_1234*` or the ttyACM with VID:PID `abcd:1234`); `send_raw(b: bytes, chunk: int|None=None)`, `send(pkt: Packet) -> int` (fills pid from one counter), `recv(timeout: float, pred=None) -> Packet|None`, `expect_silence(seconds: float) -> list[Packet]`, `close()`, `.stray` (bytes outside frames — must stay empty: the CDC carries only frames)
  - `dbg.Target("base"|"remote")` with `sym(name) -> int`, `read(addr, n) -> bytes`, `core_state() -> dict(halted, lockup, sleeping)`, `key(*events)` (break at the GUI dispatch, `call GUI::keypress(n)`, continue, `monitor resume`, detach), `gpio(port: str, pin: int) -> int` (IDR read), `debug_log() -> list[str]` (base `Debug` ring buffer)
  - fixtures in `conftest.py`: `link` (session-scoped `usblink.Link`, answers base KEEPALIVE automatically so the base sees the PC alive), `base`, `remote` (`dbg.Target`)

- [ ] **Step 1: Write the offline tests** — `test_proto_offline.py`:

```python
from proto import crc8, encode, FrameParser, MT

def test_crc_check_value():
    assert crc8(b"123456789") == 0xA2

def test_roundtrip_lengths_bytewise():
    for n in (0, 1, 127, 128):
        payload = bytes((i * 37) & 0xFF for i in range(n))
        frame = encode(1, 2, 7, MT.SIGNAL_ANCHOR, payload)
        p = FrameParser()
        got = [pkt for b in frame for pkt in p.feed(bytes([b]))]
        assert len(got) == 1 and got[0].payload == payload

def test_bad_crc_dropped():
    frame = bytearray(encode(1, 2, 1, MT.KEEPALIVE))
    frame[-2] ^= 0x55
    p = FrameParser()
    assert p.feed(bytes(frame)) == [] and p.crc_errors == 1
```

- [ ] **Step 2:** `../../.venv/bin/python -m pytest -q test_proto_offline.py` before copying `proto.py`. Expected: FAIL (`ModuleNotFoundError: proto`).
- [ ] **Step 3:** Copy `proto.py`; write `usblink.py` (pyserial, reader thread feeding `FrameParser` into a `queue.Queue`, everything outside a frame appended to `.stray`) and `dbg.py` (pattern of the remote harness's `dbg.py`, parameterised by serial/config/ELF/port; gdb with `-nx` and `monitor resume` before detach).
- [ ] **Step 4:** Offline tests PASS. Smoke on hardware: `Link().send(KEEPALIVE to Base)` → `KEEPALIVE_ACK` src 2 dst 1 within 200 ms; `Target("base").core_state()` = running; `Target("remote").core_state()` = running.
- [ ] **Step 5:** README (bench, setup with `uv venv`, run commands), commit.

### Task 3: Conformance — base as its own node over USB-CDC

**Files:** Create `tools/basetest/test_cdc_local.py`, `tools/basetest/test_cdc_robust.py`

**Interfaces:** Consumes `link`, `base` fixtures; `proto.*`.

- [ ] **Step 1: Write the tests.** Local handling (`test_cdc_local.py`):
  - `test_keepalive_ack`: KEEPALIVE PC→Base → KEEPALIVE_ACK, same pid not required, src 2.
  - `test_get_status`: GET_STATUS → ACK + STATUS_RESP with 4 bytes, `pcAlive == 1`, `bleConnected` equal to `Bluetooth::_connected` read via `base.read`.
  - `test_set_current_on_off`: SET_CURRENT `[1]` → ACK, `base.gpio("A", 4) == 1`; `[0]` → 0. Menu checkbox state (`checkboxPower()` value) follows.
  - `test_set_current_bad_payload`: empty payload → NAK `0x02`.
  - `test_unknown_type_nak`: type `0x7E` → NAK `[pid, 0x01]`.
  - `test_duplicate_pid_executed_once`: same SET_CURRENT pid twice → two ACKs, one GPIO edge (sample `base.gpio` between).
  - `test_wrong_dest_ignored`: dest `0x09` → no reply (`expect_silence(0.5)`).
  - `test_stray_is_empty`: after all of the above, `link.stray == b""` (no debug text in CDC).
- [ ] **Step 2:** Robustness (`test_cdc_robust.py`): `test_split_frame_bytewise` (1 byte per write, 5 ms apart → ACK), `test_burst_8_frames` (8 KEEPALIVE in one write → 8 ACKs), `test_bad_crc_silent`, `test_truncated_then_good` (half a frame, then a full one → only the full one answered), `test_300_byte_garbage` (then a KEEPALIVE still answered), `test_max_escaped_payload` (SIGNAL_ANCHOR to Remote with 127 bytes of `0x01/0x04/0x1B/$/%` while remote disconnected → NAK `0x06`, no fault: `base.core_state()["lockup"] is False`), `test_random_20x2k` (20 × 2 KB random → no lockup, KEEPALIVE answered after).
- [ ] **Step 3:** Run both files. Expected: PASS. Any failure → systematic-debugging, fix in `src/`, rebuild, flash `-update.hex`, rerun, commit the fix alone (`Base: <what>`).
- [ ] **Step 4:** Commit the tests.

### Task 4: Church display output

**Files:** Create `tools/basetest/test_display.py`

**Interfaces:** Consumes `link`, `la.capture`, `la.uart`, `la.CH`.

- [ ] **Step 1:** Read `src/display.cpp` to get the 9-byte frame layout; write `expected_frame(song_h, song_l, verse, letter, led) -> bytes` in the test from it (document the layout in a comment).
- [ ] **Step 2: Write the tests:**
  - `test_now_playing_frame`: capture D0/D1 4 s; send NOW_PLAYING_INFO `[0xF4, 0x06, 0xF1, 0x0A, 0x02]` (" 406", verse " 1", A, green). Expected: no ACK (PROTOCOL.md: NOW_PLAYING_INFO is not ACKed; `expect_silence(0.5)` sees no reply), and the next frame on the display TX equals `expected_frame(...)`; frames repeat every 500 ms with the same content.
  - `test_no_change_field`: then `[0xFF, 0xFF, 0xF2, 0xFF, 0xFF]` → only the verse changes to " 2".
  - `test_blank_fields`: `[0xFE, 0x00, 0xFE, 0x0F, 0x00]` → song and verse blank.
  - `test_display_burst_last_wins`: 5 NOW_PLAYING_INFO back to back; every decoded frame is 9 bytes with valid framing (no truncated frames, no UART framing errors in the decoder output), and the frame 600 ms later shows the 5th state.
  - `test_digit_shift`: song `0x51 0x3F` ("513 ") is sent as authored.
- [ ] **Step 3:** Run. Expected: PASS; fix firmware via systematic-debugging if not.
- [ ] **Step 4:** Display sense (PC13): if a display is not attached, read `display` connected flag and STATUS_RESP `displayConnected`; record the value. Ask the user to plug/unplug the display only in the manual checklist (Task 9).
- [ ] **Step 5:** Commit.

### Task 5: First-time pairing with the remote, auth unification, relay

**Files:**
- Modify: `src/bluetooth.cpp` (scan parsing, `startPairing()`, `StatusLine` size), `inc/bluetooth.hpp` (declarations), `src/main.cpp` (pairing state machine in the status-event switch), `src/menu.cpp` (menu item "Sparovat ovladac" in `settingsItems`, result messages)
- Modify (remote, only if Step 2 says so): `../midicontroller-fw/src/bluetooth.cpp` (`SA,` value)
- Create: `tools/basetest/test_relay.py`, `tools/basetest/test_pairing.py`

**Interfaces:**
- Produces (base firmware):
  - `uint8_t Bluetooth::startPairing()` — queues `Z`, `F` (scan); returns 1 when the engine is busy
  - `void Bluetooth::stopScan()` — queues `X`
  - `bool Bluetooth::connectTo(uint8_t addrType, const char *addr)` — queues `C,<type>,<addr>`
  - `Bluetooth::PairingState { IDLE, SCANNING, CONNECTING, BONDING, DONE, FAILED }` and `PairingState Bluetooth::pairingState()`
  - `static constexpr const char *REMOTE_NAME = "MIDIControlRemote";` and `static constexpr duration PAIRING_TIMEOUT = 15000_ms;`
- Consumes: `router::isRemoteAlive()`, existing `AdvConnectableCommand` (fields `addr`, `connected` = address type, `name`)

- [ ] **Step 1: See the real scan output first.** Do not clear the base's bonds in this step. With D2/D3 capturing 20 s, put the base module into scan by hand through gdb: `call Bluetooth::queueCommand("F")`, wait 10 s, `call Bluetooth::queueCommand("X")`. Decode D3 (RN4870→MCU). Record the exact advertisement lines in the plan's notes (expected form `%<12-hex addr>,<0|1>,<name>,<uuids>,<rssi>%`, name may be empty, lines may be longer than 47 chars). Read `Bluetooth::_statusQueue` contents to see what the 48-byte `StatusLine` keeps.
- [ ] **Step 2: Auth decision.** Base uses `SA,2` (NoInputNoOutput), remote `SA,1` (DisplayYesNo): the combination gives Just Works. Check DS50002466 (`SA` table) and keep it if the pairing in Step 6 completes without a passkey prompt on either side; if it does, document both values in both `CLAUDE.md` files as intentional and close the REVIEW item. Change only if pairing fails.
- [ ] **Step 3: Write the failing hardware test** `test_pairing.py`:

```python
def test_pairing_finds_remote_and_bonds(base, remote, link):
    base.call("Bluetooth::queueCommand(\"U,Z\")")       # forget all bonds on the base module
    wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 10)
    base.call("Bluetooth::startPairing()")
    wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == DONE, 20)
    assert base.read_bool("Bluetooth::_bonded")
    # link works: base keepalives reach the remote and are answered
    wait_until(lambda: remote_alive(base), 6)

def test_reconnect_after_base_reset_uses_bond(base, remote):
    base.reset()
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15)  # "C" to the bonded remote

def test_pairing_timeout_no_remote(base, remote):
    remote.hold_ble_reset(True)                         # remote RN4870 held in reset via PB8
    base.call("Bluetooth::queueCommand(\"U,Z\")")
    base.call("Bluetooth::startPairing()")
    wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == FAILED, 20)
    remote.hold_ble_reset(False)
```

  Add `call(expr)`, `read_bool`, `read_u8`, `reset()`, `hold_ble_reset(on)` (remote: set/clear PB8 through `GPIOB->BSRR` with the core running) to `dbg.Target`.
- [ ] **Step 4:** Run. Expected: FAIL (`startPairing` not defined).
- [ ] **Step 5: Implement.** Grow `StatusLine::text` to 96 bytes (scan lines are ~70 chars). In `startPairing()` queue `{"Z", "F"}`, set `SCANNING`, start a timestamp. In `main.cpp`, while `SCANNING`, on `AdvConnectableCommand` whose `name` equals `REMOTE_NAME`: `stopScan()`, `connectTo(type, addr)`, state `CONNECTING`. On `CONNECT` while `CONNECTING`: state `BONDING`, queue `B` directly (no OLED confirm: the user started pairing from the menu). On `BONDED`/`SECURED`: `DONE`, `_reconnectNeeded = false`. Timeout (`PAIRING_TIMEOUT`) in any non-final state: `X`, `K,1`, `FAILED`. Auto-reconnect is suspended while pairing. Menu: `ui::MenuItem("Sparovat ovladac", pairCallback)` in `settingsItems` before "Zpet"; screen messages "Hledam ovladac...", "Sparovano", "Ovladac nenalezen" (each ≤ 18 chars). Build, flash `-update.hex`.
- [ ] **Step 6:** Run `test_pairing.py`. Expected: PASS. Capture D2/D3 and D6/D7 during the pairing for the log (both sides `%CONNECT%`, `%STREAM_OPEN%`, `%SECURED%`/`%BONDED%`).
- [ ] **Step 7: Relay tests** `test_relay.py` (remote connected and bonded):
  - `test_base_keepalive_to_remote`: D2 (base→RN4870) shows a KEEPALIVE frame Base→Remote every 2 s ± 0.2 s for 10 s; `router::isRemoteAlive()` true.
  - `test_pc_signal_reaches_remote`: SIGNAL_PLAYING `"406A"` PC→Remote → the remote's ACK arrives on CDC with src 3 and the acked pid; remote `PlayerScreen` state = playing (`remote.player_state()`).
  - `test_remote_play_reaches_pc`: `remote.key(Digit4, Digit0, Digit6, Enter)` → PLAY_SONG src 3 dst 1 payload `b"406"` on CDC; harness ACKs it; no retransmission follows (`expect_silence(1.5)` for that pid).
  - `test_remote_retry_without_pc_ack`: same but no ACK → 4 transmissions, 0.5 s apart, same pid (proves the base forwards retries, not dedupes them away).
  - `test_signal_anchor_and_replay`: SIGNAL_ANCHOR `"Sloka 2\0"` → remote shows the anchor; `remote.key(Right)` → REPLAY_ANCHOR src 3 on CDC.
  - `test_song_list_to_remote`: remote opens the song list → GET_SONG_LIST `[0]` on CDC → harness answers SONG_LIST_RESP `[1, b"401;403;406A;406B"]` to Remote → remote ACK on CDC.
  - `test_pc_to_remote_unreachable`: `remote.hold_ble_reset(True)`; after the base sees `%DISCONNECT%`, SIGNAL_STOPPED to Remote → NAK `[pid, 0x06]` within 200 ms; release reset; base reconnects within 10 s and the next SIGNAL_STOPPED is ACKed by the remote; nothing stale from before arrives at the remote (D6/D7 decode).
  - `test_status_ble_connected`: GET_STATUS `bleConnected` tracks the link in both states.
- [ ] **Step 8:** Run `test_relay.py`. Expected: PASS; fixes via systematic-debugging.
- [ ] **Step 9:** Commit firmware (`Base: first-time pairing with the remote from the menu`) and tests separately; update `CLAUDE.md` (pairing flow, SA values).

### Task 6: USB-MIDI ↔ DIN through the Otraki interface

**Files:** Create `tools/basetest/midi.py` (`amidi` wrappers: `ports() -> dict`, `send(port, hex)`, `receive(port, seconds) -> bytes`), `tools/basetest/test_midi.py`

- [ ] **Step 1:** `amidi -l`; identify the base port (`MIDIControl`/`ABCD`) and the Otraki port (`USB MIDI Interface`). Write them in `midi.ports()` by name match.
- [ ] **Step 2: Write the tests:**
  - `test_usb_to_din_notes`: send `90 3C 64 80 3C 00` to the base port; Otraki receives the same 6 bytes (running status may compress to `90 3C 64 3C 00` — accept both, assert the decoded events).
  - `test_din_to_usb_notes`: send from Otraki; the base port receives the same events.
  - `test_din_to_usb_running_status`: Otraki sends `90 3C 64 3E 64 40 64` → three note-ons on USB.
  - `test_sysex_both_ways`: `F0 7D 01 02 03 F7` USB→DIN and DIN→USB unchanged.
  - `test_realtime_inside_sysex`: DIN `F0 7D 01 F8 02 F7` → USB carries `F8` and the intact SysEx `F0 7D 01 02 F7`.
  - `test_midi_latency_during_reconnect`: remote BLE held in reset (base retries `C` every 5 s); stream 200 notes 10 ms apart USB→DIN for 6 s; every note arrives, max inter-arrival gap < 30 ms (measure with `amidi -d` timestamps via `aseqdump` on the Otraki port).
  - `test_midi_with_display_and_cdc_traffic`: same stream while sending NOW_PLAYING_INFO every 100 ms and KEEPALIVE every 50 ms on CDC → no lost notes.
- [ ] **Step 3:** Run. Expected: PASS; fixes via systematic-debugging.
- [ ] **Step 4:** Commit.

### Task 6A: Protocol DIAG messages and bootloader watchdog kick (shared repos)

Spec: `docs/superpowers/specs/2026-10-19-diagnostics-logging-watchdog-design.md` §4, "Exception", "No-init RAM".

**Files:**
- Modify (`../midicontrol-protocol`): `inc/protocol/types.hpp` (MsgType `DIAG_GET = 0x80`, `DIAG_RESP = 0x81`), `PROTOCOL.md` (message table, payloads, record layout, code table, polling rule, NAK UNKNOWN_MSG from old firmware), new `inc/protocol/diag.hpp` (record + codes, header-only)
- Modify (`../midicontrol-bootloader`): add `inc/boot/wdg.hpp` with `boot::wdg::kick()` (`if (IWDG running) IWDG->KR = 0xAAAA`) and call it inside `flashprog::eraseRange()` / `program()` loops and any receive loop in the shared engine
- Modify (`midicontrol-fw`): `bootloader/src/main.cpp` main loop calls `boot::wdg::kick()`; `bootloader/STM32G431CBTx_BOOT.ld` and `STM32G431CBTx_FLASH.ld`: `RAM LENGTH = 30K`, new `NOINIT (rw) : ORIGIN = 0x20007800, LENGTH = 2K` and an output section `.noinit (NOLOAD) : { *(.noinit*) } >NOINIT`; `.mcu-profile.json` `sizeBudget.ram` = 29184; bump `lib/protocol`, `lib/bootloader`
- Modify (`../midicontroller-fw`): `bootloader/src/main.cpp` main loop + its W25Q32→internal copy loop call `boot::wdg::kick()`; bump `lib/protocol`, `lib/bootloader`
- Modify (`../midicontrolpy-sw`): vendored `include/midicontrol/protocol/` + `src/protocol/` get the same types and `diag.hpp`; `src/protocol/names.cpp` names `DIAG_GET`/`DIAG_RESP`; test in `tests/test_names.cpp`
- Test: `tools/basetest/test_proto_offline.py` (DIAG_RESP encode/parse roundtrip with 8 records = 101-byte payload)

**Interfaces (produced):**
```cpp
// protocol/diag.hpp
namespace protocol::diag {
struct Record { uint16_t seq; uint8_t code; uint8_t level; uint32_t uptimeMs; uint32_t arg; };   // 12 B LE on the wire
static constexpr size_t RECORD_SIZE = 12, MAX_RECORDS = 8;
enum class Code : uint8_t { BOOT = 1, FAULT = 2, FAULT_DETAIL = 3, LOOP_STALL = 4, IWDG_RESET = 5, BLE_CONNECT = 6,
	BLE_DISCONNECT = 7, BLE_SECURED = 8, LINK_LOST = 9, RETRIES_EXHAUSTED = 10, CRC_ERRORS = 11, CHARGER_FAULT = 12,
	LOW_BATTERY = 13, OTA_RESULT = 14, SHUTDOWN = 15, RECORDS_LOST = 16 };
enum class Level : uint8_t { ERROR = 1, WARNING = 2, INFO = 3, DEBUG = 4 };
void encodeRecord(const Record &r, uint8_t *out12);
Record decodeRecord(const uint8_t *in12);
} // DIAG_GET payload [cursor u16 LE]; DIAG_RESP payload [boot_id u16, next_cursor u16, n u8, n × 12 B]
namespace boot::wdg { void kick(); }   // no-op when the IWDG was never started
```

- [ ] **Step 1:** Write the offline roundtrip test in `test_proto_offline.py` (records with seq 0..7, all codes, arg 0xDEADBEEF; payload length 5 + 8·12 = 101; escaped bytes inside records survive `FrameParser` byte-wise feeding). Run: FAIL (`MT.DIAG_RESP` missing). Add `DIAG_GET/DIAG_RESP` and the record codec to `tools/basetest/proto.py` (it mirrors the protocol; note in its header). Run: PASS.
- [ ] **Step 2:** Protocol repo: `types.hpp`, `diag.hpp` (+ a host unit test in the protocol repo if it has a test target; otherwise rely on the PC tests in Step 5), `PROTOCOL.md`. Commit there (`Protocol: DIAG_GET/DIAG_RESP diagnostics records`).
- [ ] **Step 3:** Bootloader repo: `boot::wdg::kick()` + calls; build both bootloaders (`mcu build` in each firmware). Commit there (`Bootloader: kick the IWDG in long loops`).
- [ ] **Step 4:** Firmware repos: linker scripts (base only), `bootloader/src/main.cpp` kicks (both), submodule bumps, `sizeBudget.ram`. `mcu build` + `mcu size` in both: PASS within budget. Check `arm-none-eabi-nm` that the base app's and bootloader's `_estack` = `0x20007800` and nothing but `.noinit` lies in 0x20007800..0x20008000. Flash the base `-full.hex` (bootloader changed) and the remote `-full.hex`? — **the remote only after asking the user** (bootloader reflash); the base: back up first (`../backups/base-g431-flash-<date>-pre-diag.bin`). Rerun `test_cdc_local.py`: PASS.
- [ ] **Step 5:** PC repo: vendored protocol + names + `test_names.cpp` case; `cmake --build build && ./build/midicontrol_tests`: all pass. Commit.
- [ ] **Step 6:** Commit each firmware repo (`Base: protocol + bootloader bump for diagnostics, no-init RAM block`, `Remote: …`). Do not push.

### Task 6B: Base text log on a second USB-CDC

Spec §1.

**Files:**
- Modify: `inc/tusb_config.h` (`CFG_TUD_CDC 2`), `src/usb_descriptors.c` (second CDC IAD + interfaces after MIDI, endpoints 0x84 notif / 0x05 OUT / 0x85 IN, interface string "MIDIControl log"), `src/usb.cpp` (configure CDC 1 like CDC 0), `inc/debug.hpp`, `src/debug.cpp` (rework), `src/main.cpp` (call `Debug::process()` in the loop, `Debug::banner()` after USB init), call sites in `src/bluetooth.cpp`, `src/router.cpp`, `src/display.cpp`, `src/midi.cpp`, `src/settings.cpp`, `src/usb.cpp`
- Create: `inc/resetcause.hpp`, `src/resetcause.cpp` (`RCC->CSR` decode + RMVF), `tools/basetest/loglink.py`, `tools/basetest/test_log.py`
- Modify: `tools/basetest/usblink.py` (pick the **protocol** interface explicitly: sysfs `bInterfaceNumber == 00`; never the log CDC), `tools/basetest/README.md`

**Interfaces (produced):**
```cpp
namespace Debug {
enum class Level : uint8_t { NONE = 0, ERROR, WARNING, INFO, DEBUG };
void log(Level lvl, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));  // main-loop context only
void logFromIsr(uint8_t eventCode);   // sets a flag/counter; process() turns it into a line
void init();                           // validates the .noinit ring header; keeps pre-reset contents when valid
void banner();                         // name, git revision/date, reset cause, saved fault record (Task 6C fills it)
void process();                        // drains the ring to CDC 1 when DTR is set; parses "level x", "dump", "help"
Level getLevel(); void setLevel(Level);
}
namespace resetcause { const char *describe(uint32_t csr); uint32_t readAndClear(); }   // flags joined by '|'
```
```python
# tools/basetest/loglink.py
class LogLink:                      # opens the log CDC (bInterfaceNumber of the "MIDIControl log" interface)
    def __init__(self, port=None): ...
    def lines(self, timeout: float, pred=None) -> list[str]   # complete lines received within timeout
    def wait_for(self, regex: str, timeout: float) -> re.Match
    def command(self, text: str) -> list[str]                 # writes text + "\r\n", returns reply lines
    def close(self): ...
```
Line format (exact): `[%6lu.%03lu] %c %s: %s\r\n` → e.g. `[    12.345] W ble: cmd failed: C`.

- [ ] **Step 1: Write the failing hardware tests** `test_log.py`:
  - `test_log_cdc_is_separate`: two ttyACM for `abcd:1234`; `usblink.Link` opens interface 00 and still gets KEEPALIVE_ACK; `LogLink` opens the other; `link.stray == b""` after 2 s of logging.
  - `test_banner_after_probe_reset`: probe reset → USB back within 5 s → `wait_for(r"I sys: MIDIControl base .* rev [0-9a-f]{7}")` and `wait_for(r"reset: .*PIN")`.
  - `test_banner_after_software_reset`: `base.call("NVIC_SystemReset()")` → `reset: .*SOFTWARE`.
  - `test_level_switch`: `command("level d")` → reply `level D`; a GET_STATUS on the protocol link produces a `D router:` line; `command("level i")` → none after.
  - `test_pre_reset_log_survives`: note the last `I`/`W` line, probe reset, then `wait_for("--- log before reset ---")` followed by that line.
  - `test_ring_wraps_without_host` (Review Focus): close the log port, generate > 3 KB of D-level lines for 10 s (`level d`, 50 GET_STATUS/s), core never stalls (`core_state`, KEEPALIVE answered within 200 ms throughout), reopen → a `[dropped N]` marker with N > 0 and complete lines after it.
  - `test_line_format`: every received line matches `^\[ *\d+\.\d{3}\] [EWID] [a-z]{1,6}: .{0,120}~?$`.
- [ ] **Step 2:** Run: FAIL (one CDC only).
- [ ] **Step 3:** Implement descriptors + second CDC; `usblink.py` interface selection; build; check PMA allocation and `mcu size` (≤ 29184 data+bss). Flash `-update.hex`. `lsusb -v -d abcd:1234` shows 2 CDC + MIDI; `test_cdc_local.py` PASS.
- [ ] **Step 4:** Implement `Debug` rework (ring in `.noinit` with `{magic 0x4C4F4721, head, tail, dropped, crc32 of the 4 fields}`, 1536-byte ring), `resetcause`, banner, commands, call sites (each module tag ≤ 6 chars: `sys ble router usb disp midi set ota`). Czech comments. Never block: when the CDC TX FIFO is full, stop draining until next loop pass.
- [ ] **Step 5:** Run `test_log.py` + `test_cdc_local.py` + `test_cdc_robust.py` + `test_display.py`: PASS.
- [ ] **Step 6:** Commit firmware (`Base: text log on a second USB-CDC, reset cause, pre-reset log`) and tests separately.

### Task 6C: Base fault capture, loop watchdog and IWDG

Spec §2, §3.

**Files:**
- Create: `inc/fault.hpp`, `src/fault.cpp`, `inc/wdg.hpp`, `src/wdg.cpp`, `tools/basetest/test_fault.py`
- Modify: `src/main.cpp` (fault handlers → `fault::capture`, `SysTick_Handler` → `wdg::onTick` via a naked wrapper that passes the stacked frame, `wdg::start()` after clocks, `wdg::kick()` once per loop pass, `signalError()` → record + 3 s blink + reset), `src/base.cpp` (replace dead `wdtStart()`), blocking spots: `src/bluetooth.cpp` (init/`$$$` busy-waits), `src/oled.cpp` (`flush` wait), `src/midi.cpp` (DIN busy-wait) call `wdg::kick()`; `src/debug.cpp` banner prints the record

**Interfaces (produced):**
```cpp
namespace fault {
enum class Kind : uint8_t { HARDFAULT = 1, MEMMANAGE, BUSFAULT, USAGEFAULT, ERROR, LOOP_STALL };
struct Record {   // in .noinit, guarded by magic 0x46415554 + crc32
	uint32_t magic; uint8_t kind; uint8_t count; uint16_t errorCode;
	uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr, excReturn, msp, psp, ipsr, cfsr, hfsr, mmfar, bfar, afsr, uptimeMs;
	char rev[8]; uint32_t crc;
};
[[noreturn]] void capture(Kind kind, const uint32_t *stackedFrame, uint32_t excReturn, uint16_t errorCode = 0);
const Record *saved();      // nullptr when none/invalid
void clear();
}
namespace wdg {
void start();               // IWDG ~2 s (LSI 32 kHz: PR /32, RLR 2000) + loop watchdog armed
void kick();                // IWDG->KR = 0xAAAA and loop counter = 0
void onTick(const uint32_t *stackedFrame, uint32_t excReturn);   // from SysTick; at 1000 ms → fault::capture(LOOP_STALL, …)
}
```
Debugger attached (`CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk`) → `bkpt` after recording, else `NVIC_SystemReset()`. Note: the IWDG keeps counting while the core is halted unless `DBGMCU->APB1FZR1 |= DBG_IWDG_STOP` — set it in `wdg::start()` so gdb sessions do not reset the base (and keep the harness's halts short anyway).

- [ ] **Step 1: Write the failing hardware tests** `test_fault.py` (each starts from a running base and ends with USB + log back):
  - Fault injection hook (kept in release builds, it costs one byte and one compare per loop pass): `[[gnu::used]] volatile uint8_t fault::testInject` read once per main-loop pass — `1` = read from 0xFFFFFFF0 (bus fault → HardFault), `2` = spin forever with interrupts on (loop stall), `3` = spin with interrupts off (IWDG). The test writes it with `base.write()` while the core runs, then **disconnects the probe from the debug session** (`monitor resume`, gdb detach, stop openocd) so DHCSR.C_DEBUGEN is 0 and the release path (reset) runs.
  - `test_hardfault_record`: `testInject = 1` → base resets → banner shows `E fault: HARDFAULT` (or `BUSFAULT`) with `pc=` that `arm-none-eabi-addr2line -f -e build/midicontrol-fw.elf` resolves into the main-loop function, `cfsr=` decoded with `PRECISERR`/`IBUSERR`/`BFARVALID` names, and the pre-crash log lines under `--- log before reset ---`.
  - `test_loop_stall_record`: `testInject = 2` → after ~1 s a `LOOP_STALL` record whose pc resolves to the spin; reset cause SOFTWARE.
  - `test_iwdg_reset`: `testInject = 3` → reset cause `IWDG` in the banner after ~2 s.
  - `test_no_bogus_fault_after_garbage` (Review Focus): write random bytes over the whole 2 KB `.noinit` block through gdb, probe reset → banner shows no fault record and the log starts fresh (header CRC rejected).
  - `test_debugger_halts_not_resets`: with gdb attached and C_DEBUGEN set, `testInject = 1` → core halted at `bkpt`, record present in RAM; resume via reset afterwards.
  - `test_long_operations_do_not_trip`: BLE reconfigure (`Bluetooth::configure()` via gdb call → `$$$` sequence), OLED full flush, 200 DIN notes → no reset (USB stays, uptime keeps growing in log timestamps).
- [ ] **Step 2:** Run: FAIL (no `fault::testInject`).
- [ ] **Step 3:** Implement `fault`, `wdg`, handler wiring, kicks in blocking spots, `testInject` handling in the loop. Build, `mcu size`, flash `-update.hex`.
- [ ] **Step 4:** Run `test_fault.py`, then the full `-m hw` suite (Tasks 3–6 + `test_log.py`): PASS. Run `test_pairing.py` (BLE init and pairing under the watchdog).
- [ ] **Step 5:** Commit firmware (`Base: fault records, loop watchdog and IWDG`) and tests separately.

### Task 6D: Remote diagnostics records, fault capture and watchdogs

Spec §3, §4. Remote repo `../midicontroller-fw`. **Before flashing the remote, ask the user** (and to hold ON/OFF if needed); a watchdog or fault reset turns the remote off — tests that trigger one ask the user to switch it back on.

**Files (remote repo):**
- Create: `inc/diag.hpp`, `src/diag.cpp` (ring of 32 `protocol::diag::Record`, `boot_id`, W25Q32 persistence in sector **0x3FF000** — assert it lies outside `kStageHeaderAddr`/`kStageImageAddr` ranges), `inc/wdg.hpp`, `src/wdg.cpp` (IWDG ~4 s: PR /64, RLR 2000; loop watchdog 2000 ms), `inc/fault.hpp`, `src/fault.cpp` (Cortex-M0+: HardFault only; stacked frame + IPSR; synchronous polling-SPI program of one slot, no erase)
- Modify: `src/main.cpp` (handlers, SysTick hook, kick per loop pass, `diag::record` at boot with reset cause), `src/link.cpp` (handle `DIAG_GET` → ACK + `DIAG_RESP`; record LINK_LOST/RETRIES_EXHAUSTED/CRC_ERRORS), `src/bluetooth.cpp` (CONNECT/DISCONNECT/SECURED), `src/bq24195.cpp`/`src/battery.cpp` (CHARGER_FAULT, LOW_BATTERY), `src/fwupdate.cpp` (OTA_RESULT; kick during erase/staging), `.mcu-profile.json` if budgets move
- Test (base repo): `tools/basetest/test_remote_diag.py`

**Interfaces (produced):**
```cpp
namespace diag {
void init();                                                   // loads unfetched W25Q32 slots into the ring, boot record
void record(protocol::diag::Code code, protocol::diag::Level lvl, uint32_t arg);   // main-loop context
void recordFromIsr(protocol::diag::Code code, uint32_t arg);   // lock-free single-slot mailbox drained in process()
void process();
// fills resp payload for a DIAG_GET with `cursor`; marks persisted slots fetched when passed
size_t handleGet(uint16_t cursor, uint8_t *payload, size_t maxLen);
uint16_t bootId();
}
```
Rate rule: at most one CRC_ERRORS record per 10 s (count in `arg`); DEBUG level never recorded.

- [ ] **Step 1: Write the failing hardware tests** `test_remote_diag.py` (through the base, `link` fixture, remote paired):
  - `test_diag_get_boot_record`: DIAG_GET `[0,0]` to Remote → ACK + DIAG_RESP with `n ≥ 1`, first record code BOOT, `level` INFO.
  - `test_diag_cursor_paging`: generate ≥ 10 records (BLE disconnect/reconnect via `remote.hold_ble_reset` twice) → successive DIAG_GETs with `next_cursor` return each seq exactly once, n ≤ 8 per response.
  - `test_diag_old_unknown_rejected`: covered by protocol: DIAG_GET to the **Base** → NAK `[pid, 0x01]` (base has no DIAG; log CDC instead).
  - `test_remote_fault_survives_power_off` (needs the user): `remote.write(fault::testInject, 1)` → remote powers off; ask the user to switch it on; after reconnect DIAG_GET returns FAULT + FAULT_DETAIL with a pc inside the remote's loop (addr2line on the remote ELF) and BOOT with reset cause.
  - `test_remote_loop_stall`: `testInject = 2` → same flow, LOOP_STALL record.
  - `test_diag_traffic_budget` (Review Focus): 60 s with the PC poll emulated every 10 s → ≤ 6 DIAG_GET and ≤ 6 DIAG_RESP frames on D6/D7, each ≤ 120 B on the wire.
  - `test_ota_under_watchdog` is in Task 7 (`test_remote_ota_via_base` runs with the IWDG on).
- [ ] **Step 2:** Run: FAIL (DIAG_GET NAKed UNKNOWN_MSG by the remote).
- [ ] **Step 3:** Implement; host unit tests for the ring/cursor logic in `src/logic/` (`test/host/`), `-DHOST_TESTS=ON`: PASS.
- [ ] **Step 4:** Build, ask the user, flash the remote `-update.hex`, run `test_remote_diag.py` + `test_relay.py` + `test_pairing.py` + the remote's own `tools/bletest` offline tests: PASS.
- [ ] **Step 5:** Commit remote firmware (`Remote: diagnostics records, fault capture and watchdogs`) and base tests separately.

### Task 6E: PC software — log reader, DIAG polling, store and Diagnostics tab

Spec §5. Repo `../midicontrolpy-sw` (C++17, `cmake -S . -B build && cmake --build build -j && ./build/midicontrol_tests`).

**Files:**
- Create: `include/midicontrol/diag_store.hpp`, `src/diag_store.cpp` (JSONL append/rotate 1 MB × 3/load tail), `include/midicontrol/log_line.hpp`, `src/log_line.cpp` (parse base lines), `include/midicontrol/fault_decode.hpp`, `src/fault_decode.cpp` (CFSR/HFSR bit names, fault line parsing), `src/diag_codes.cpp` (remote code → text), `src/log_reader.cpp` (+ header; thread on the log CDC), tests `tests/test_log_line.cpp`, `tests/test_diag_store.cpp`, `tests/test_fault_decode.cpp`, `tests/test_diag_resp.cpp`, `tests/test_http_diag.cpp`
- Modify: `src/serial_port.cpp` (`findMidiControlPort` picks interface 00; new `findMidiControlLogPort` by interface string/number), `src/comm.cpp` (DIAG_GET on remote link-up + every 10 s while alive, not during `FW_UPDATE_*`; DIAG_RESP → store; stop polling a node after NAK UNKNOWN_MSG until reconnect), `src/http.cpp` (`GET /api/diag`), `src/status_json.cpp` (`last_fault`, `unseen_faults`, schema 2), `src/status.cpp` (amber dot rule), `src/web_page.cpp` (Diagnostics tab), `src/config.cpp` (`[DIAG] LogPort=`, `Dir=`), `src/console_exec.cpp` (`diag` command), `CMakeLists.txt`, `config.conf` (example), `CLAUDE.md`/README

**Interfaces (produced):**
```cpp
struct LogLine { uint64_t uptimeMs; char level; std::string tag, text; };
std::optional<LogLine> parseLogLine(std::string_view line);
struct DiagEntry { int64_t pcTimeMs; std::string node; uint32_t boot; uint64_t uptimeMs; char level;
                   std::string tag, text; std::optional<FaultInfo> fault; uint64_t id; };
class DiagStore { public: explicit DiagStore(std::string dir); void append(DiagEntry e);
                  std::vector<DiagEntry> query(const std::string &node, char minLevel, uint64_t beforeId, size_t limit) const;
                  std::optional<DiagEntry> lastFault(const std::string &node) const; size_t unseenFaults(const std::string &node) const;
                  void ack(uint64_t id); };
struct FaultInfo { std::string kind; uint32_t pc, lr, cfsr, hfsr; std::vector<std::string> cfsrBits, hfsrBits; std::string rev; };
std::vector<std::string> decodeCfsr(uint32_t); std::vector<std::string> decodeHfsr(uint32_t);
```
`/api/diag?node=base|remote&level=E|W|I|D&before=<id>&limit=<n≤200>&ack=<id>` → `{"schema":1,"entries":[…],"next_before":<id|null>}`.

- [ ] **Step 1: Write the failing unit tests** (exact cases): `parseLogLine("[    12.345] W ble: cmd failed: C")` → {12345, 'W', "ble", "cmd failed: C"}; malformed lines (`""`, `"[12.3] X"`, 300-char line, a line ending `~`) → nullopt or text kept with `truncated` flag; `decodeCfsr(0x00008200)` → {"PRECISERR","BFARVALID"}; `decodeHfsr(0x40000000)` → {"FORCED"}; DIAG_RESP payload with 3 records → 3 entries with codes mapped to text, unknown code → `"code 0x7F"`; `DiagStore`: append 10 k entries → rotation keeps ≤ 3 files ≤ 1 MB each, reload returns the newest entries in order, ids monotonic across restarts; `query` filters node/level/paging; `/api/diag` JSON shape and `limit` clamp; `/api/status` has `last_fault` + `unseen_faults` and dot amber while > 0, cleared after `ack`; web page contains `Diagnostics` and fetches `/api/diag`. Also (Review Focus): `test_log_reader_reconnect` — feed a fake port that disconnects mid-line and reconnects: the partial line is dropped, no duplicate lines, no crash.
- [ ] **Step 2:** Run: FAIL (missing symbols).
- [ ] **Step 3:** Implement pure parts (`log_line`, `fault_decode`, `diag_codes`, `diag_store`) in `midicontrol_core`; run tests: PASS.
- [ ] **Step 4:** Implement app parts (log reader thread with heartbeat for the systemd watchdog rule, comm DIAG polling, HTTP route, status fields, page tab, console `diag`, config); run all tests: PASS (count reported).
- [ ] **Step 5: Hardware end to end:** run the player headless against the bench (config from Task 8 Step 1, `[DIAG] Dir` in the scratchpad), `curl /api/diag?node=base` shows the banner lines; trigger a base fault (`test_fault.py::test_hardfault_record` with the player running instead of the harness log reader — the log CDC can only be open once) → `/api/status` base dot amber, `/api/diag` shows the decoded fault; trigger a remote fault (user switches it back on) → remote entry appears within 20 s of reconnect. Screenshot the Diagnostics tab via headless browser if available, else save the `/api/diag` JSON in the report.
- [ ] **Step 6:** Commit (`Diagnostics: base log CDC reader, remote DIAG polling, store and web tab`). Do not push.

### Task 7: Link lifecycle, OTA and soak

**Files:** Create `tools/basetest/test_lifecycle.py`, `tools/basetest/test_ota.py`, `tools/basetest/test_soak.py`

- [ ] **Step 1: Lifecycle tests:**
  - `test_pc_keepalive_loss`: stop answering/sending on CDC for 7 s → STATUS (read via gdb `router::isPcAlive()`) false; resume → true.
  - `test_base_reset_cdc_returns`: reset the base through the probe → `/dev/ttyACM*` disappears and returns within 5 s; `Link` reopens; KEEPALIVE answered; remote link back within 15 s.
  - `test_remote_power_cycle_midsong`: SIGNAL_PLAYING to remote; hold remote BLE in reset 10 s; release → remote link back, next SIGNAL_ANCHOR ACKed.
  - `test_rn4870_reset_on_base`: pulse base PA1 (RN4870 RST) low 50 ms with the core running → base reconfigures or reconnects; link back within 15 s.
- [ ] **Step 2: OTA:** (all three run with the IWDG and loop watchdog from Tasks 6C/6D active on both boards — this is the check that the bootloader kicks work; the base log CDC must show the update and the reset cause after it)
  - `test_remote_ota_via_base`: build `../midicontroller-fw` `-update.bin`; run the PC updater (`midicontrolpy-sw` `update remote <file>`) against the base's CDC (stop the harness link first). Expected: all chunks ACKed through the base, remote reboots into the new image, `verify_image` OK, link back. Record duration.
  - `test_base_ota_over_cdc`: `update base build/midicontrol-fw-update.bin` → ENTER_BOOTLOADER, `abcd:1235` appears, image written, app restarts, `abcd:1234` back, firmware `inc/git.hpp` hash in the version string matches.
  - `test_base_ota_power_loss`: start a base update, reset the base through the probe mid-transfer → board stays in the bootloader (`abcd:1235`) and a second update succeeds (REVIEW bootloader bug 1 regression).
- [ ] **Step 3: Soak** `test_soak.py` (`SOAK_SECONDS`, default 600): loop PLAY from the remote (key injection) → harness answers SIGNAL_PLAYING, 3 × SIGNAL_ANCHOR, NOW_PLAYING_INFO, MIDI notes on USB, SIGNAL_STOPPED. Count missed ACKs, CRC errors, lost notes, keepalive misses (both links). Expected: all zero; both cores never halted/locked.
- [ ] **Step 4:** Run all three files. Fixes via systematic-debugging.
- [ ] **Step 5:** Commit.

### Task 8: End to end with the real PC player

**Files:** none (results go to REVIEW.md in Task 9)

- [ ] **Step 1:** Build `midicontrolpy-sw` (`cmake -S . -B build && cmake --build build`), run its unit tests (expected 83/83). Write a bench `config.conf` in the scratchpad with `PlayFolder` = the repo's songs, MIDI port = the base's port, `WarmupMs=500`.
- [ ] **Step 2:** Start the player headless with that config. On the remote (key injection) enter `406A` + Enter. Expected: PLAY_SONG reaches the PC; SSR PA4 goes high; MIDI notes appear on the Otraki port (`aseqdump`), the display frame shows ` 406` · ` 1` · A; the remote shows playing and the verse anchor; at the end SIGNAL_STOPPED, display cleared, SSR follows `KeepPowerOn`.
- [ ] **Step 3:** Add verse: during playback press the remote's add-verse key → the PC replays the section (notes repeat on Otraki), verse number unchanged.
- [ ] **Step 4:** Stop from the remote mid-song → notes stop within 100 ms, display cleared.
- [ ] **Step 5:** All 11 songs once each from the remote's song list (523 with its empty anchor falls back to the file name). Record any deviation.

### Task 9: Leftovers, manual checklist and documentation

**Files:** Modify `../REVIEW.md`, `CLAUDE.md`, `../midicontroller-fw/CLAUDE.md`; optionally `lib/ui` (submodule, both firmwares)

- [ ] **Step 1: stmcpp-ui Czech defaults** (REVIEW open question 11): in the `lib/ui` upstream repo change `ConfirmScreen` "Yes"/"No" → "Ano"/"Ne", `MenuItem::back()` default → "Zpet", NumberScreen hints to Czech (≤ 18 chars); commit there, bump `lib/ui` in both firmwares, build both, flash both, rerun `test_pairing.py` and the remote's `test_outbound.py`. Ask the user before pushing the submodule upstream.
- [ ] **Step 2: Manual checklist for the user** (one message, all at once): OLED shows the menu and the pairing screens; encoder turn/push; "Sparovat ovladac" from the menu with the real remote; display unplug/replug (PC13 hot-plug → STATUS_RESP `displayConnected`); remote ON/OFF short press (no action) and 5 s hold (shutdown) with the new 20 ms debounce; SSR output with the installed relay.
- [ ] **Step 3:** REVIEW.md: new hardware test log section for 2026-10-19 (bench, suites and pass counts, findings numbered F5…), tick the Phase 1 items that are now verified (smoke test, RN4870 command set), update the base table. Both CLAUDE.md files: bench facts that changed (ST-Links by serial, analyzer map). Document diagnostics in both CLAUDE.md files and `midicontrolpy-sw/CLAUDE.md`: log CDC, line format, `level`/`dump` commands, fault records and how to `addr2line` them, watchdog periods and kicks, DIAG_GET/DIAG_RESP, the Diagnostics tab; REVIEW.md: close the `bkpt` freeze item (line ~177) as fixed.
- [ ] **Step 4:** Commit docs; push base, remote and protocol repos only after the user says so.
