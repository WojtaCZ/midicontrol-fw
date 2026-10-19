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

## Review Focus

1. **Short writes and frames split across USB packets** (CDC delivers 64-byte packets; the PC may write 1 byte at a time). Parser must reassemble; replies must not interleave. Pinned in Task 3 (`test_split_frame_bytewise`, `test_burst_8_frames`).
2. **Remote unreachable while the PC sends to it** (BLE down). The base must answer `NAK UNREACHABLE (0x06)` at once and not queue stale packets that fire on reconnect. Pinned in Task 5 (`test_pc_to_remote_unreachable`).
3. **USB-MIDI traffic during BLE reconnect attempts and display frames** (the old code busy-waited ~1 s). Notes must keep timing. Pinned in Task 6 (`test_midi_latency_during_reconnect`).
4. **Display updates faster than a 75 ms frame** (several `NOW_PLAYING_INFO` in a row). Each frame on the wire must be complete; the last state must win. Pinned in Task 4 (`test_display_burst_last_wins`).
5. **Pairing with the remote when other BLE devices advertise nearby** (phones, the PC). The base must connect only to `MIDIControlRemote` and give up cleanly after a timeout. Pinned in Task 5 (`test_pairing_timeout_no_remote`) and the manual scan capture in Task 5 Step 1.

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

### Task 7: Link lifecycle, OTA and soak

**Files:** Create `tools/basetest/test_lifecycle.py`, `tools/basetest/test_ota.py`, `tools/basetest/test_soak.py`

- [ ] **Step 1: Lifecycle tests:**
  - `test_pc_keepalive_loss`: stop answering/sending on CDC for 7 s → STATUS (read via gdb `router::isPcAlive()`) false; resume → true.
  - `test_base_reset_cdc_returns`: reset the base through the probe → `/dev/ttyACM*` disappears and returns within 5 s; `Link` reopens; KEEPALIVE answered; remote link back within 15 s.
  - `test_remote_power_cycle_midsong`: SIGNAL_PLAYING to remote; hold remote BLE in reset 10 s; release → remote link back, next SIGNAL_ANCHOR ACKed.
  - `test_rn4870_reset_on_base`: pulse base PA1 (RN4870 RST) low 50 ms with the core running → base reconfigures or reconnects; link back within 15 s.
- [ ] **Step 2: OTA:**
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
- [ ] **Step 3:** REVIEW.md: new hardware test log section for 2026-10-19 (bench, suites and pass counts, findings numbered F5…), tick the Phase 1 items that are now verified (smoke test, RN4870 command set), update the base table. Both CLAUDE.md files: bench facts that changed (ST-Links by serial, analyzer map).
- [ ] **Step 4:** Commit docs; push base, remote and protocol repos only after the user says so.
