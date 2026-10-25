"""The base connects and bonds only with the controller (Review Focus 5), executes frames only from a secured controller
(final review I1) and the command-mode state machine does not lose commands.

Foreign device: the PC (BlueZ) can connect to the base only when the base advertises (normally it does not, see the report) and BlueZ
drops the link right away, so %STREAM_OPEN% never arrives. Therefore the module status messages are injected via the probe into
Bluetooth::_statusQueue (dbg.inject_status) - the firmware reacts to them the same as to real ones; the base's replies really
go to the RN4870 and are visible on D3."""

import time

import pytest

import dbg
import la
import proto
from conftest import remote_alive, wait_until
from proto import ADDR_BASE, ADDR_REMOTE, MT

STRANGER = "10B1DF04F934"  # address of the PC's BT adapter
REMOTE = "D88039FE5996"
BAUD = 115200


def tx_text(sr) -> bytes:
    return bytes(b for b in la.uart(sr, la.CH["base_ble_tx"], BAUD) if 32 <= b < 127 or b == 13)


@pytest.fixture
def remote_away(base, remote_probe):
    """Remote RN4870 in reset, base disconnected; at the end release it and wait for a secured reconnection."""
    remote_probe.hold_ble_reset(True)
    try:
        wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 10, what="base disconnected")
        yield base
    finally:
        remote_probe.hold_ble_reset(False)
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="base reconnected to the remote")
    wait_until(lambda: base.read_bool("Bluetooth::_bonded"), 3, what="reconnected link secured")
    wait_until(lambda: remote_alive(base), 6, interval=0.5, what="remote alive")


def test_inbound_stranger_dropped_not_secured(remote_away):
    base = remote_away
    proc, sr = la.start(7, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        base.inject_status(f"CONNECT,0,{STRANGER}")
        time.sleep(0.3)
        base.inject_status("STREAM_OPEN")
        time.sleep(2.0)
        bonded = base.read_bool("Bluetooth::_bonded")
    finally:
        la.finish(proc, sr, 30)
        base.inject_status("DISCONNECT")  # firmware state back (the module was not actually connected)
    tx = tx_text(sr)
    assert b"K,1\r" in tx, f"stranger not disconnected: {tx!r}"
    assert b"B\r" not in tx, f"stranger got B (silent bond): {tx!r}"
    assert not bonded


RX_RING_SIZE = 512  # Bluetooth::_rxRing: RingBuffer<uint8_t, 512> = 512 B of data, _head (u32), _tail (u32)


def inject_ble_data(base, data: bytes):
    """Base: injects bytes into Bluetooth::_rxRing as if they came from the RN4870 in data mode (process() splits them)."""
    addr = base.sym("Bluetooth::_rxRing")
    with base.safe_halt():
        head = int.from_bytes(base.read(addr + RX_RING_SIZE, 4), "little")
        first = data[: RX_RING_SIZE - head]
        base.write(addr + head, first)
        if len(first) < len(data):
            base.write(addr, data[len(first):])
        head = (head + len(data)) % RX_RING_SIZE
        base.write(addr + RX_RING_SIZE, head.to_bytes(4, "little"))


def test_frames_need_secured_remote(remote_away):
    """Spoofed controller address: %CONNECT% with the controller's address + %STREAM_OPEN%, but without %SECURED% -> SET_CURRENT
    from BLE is not executed (PA4 unchanged), %ERR_SEC% -> K,1. Positive check: after %SECURED% on a link with the correct
    address the same injected frame is executed (the injection path works)."""
    base = remote_away
    ssr0 = base.gpio("A", 4)
    proc, sr = la.start(9, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    accepted_before = accepted_after = None
    ssr_unsecured = ssr_secured = None
    try:
        base.inject_status(f"CONNECT,0,{REMOTE}")
        time.sleep(0.3)
        base.inject_status("STREAM_OPEN")
        time.sleep(1.0)  # B goes to the module (not actually connected), no %SECURED%
        inject_ble_data(base, proto.encode(ADDR_REMOTE, ADDR_BASE, 0x61, MT.SET_CURRENT, bytes([1 - ssr0])))
        time.sleep(0.5)
        ssr_unsecured = base.gpio("A", 4)
        accepted_before = base.read_bool("Bluetooth::_peerAccepted")
        base.inject_status("ERR_SEC")
        time.sleep(1.5)
        # Positive check on a new connection
        base.inject_status(f"CONNECT,0,{REMOTE}")
        time.sleep(0.3)
        base.inject_status("STREAM_OPEN")
        time.sleep(1.0)
        base.inject_status("SECURED")
        time.sleep(0.3)
        accepted_after = base.read_bool("Bluetooth::_peerAccepted")
        inject_ble_data(base, proto.encode(ADDR_REMOTE, ADDR_BASE, 0x62, MT.SET_CURRENT, bytes([1 - ssr0])))
        time.sleep(0.5)
        ssr_secured = base.gpio("A", 4)
    finally:
        la.finish(proc, sr, 30)
        if base.gpio("A", 4) != ssr0 and base.read_bool("Bluetooth::_peerAccepted"):
            inject_ble_data(base, proto.encode(ADDR_REMOTE, ADDR_BASE, 0x63, MT.SET_CURRENT, bytes([ssr0])))
            time.sleep(0.5)
        base.inject_status("DISCONNECT")  # firmware state back (the module was not actually connected)
    tx = tx_text(sr)
    assert ssr_unsecured == ssr0, "SET_CURRENT from an unsecured link was executed"
    assert accepted_before is False, "peer accepted without %SECURED%"
    assert b"K,1\r" in tx, f"%ERR_SEC% outside pairing did not disconnect: {tx!r}"
    assert accepted_after is True, "secured link to the stored remote not accepted"
    assert ssr_secured == 1 - ssr0, "frame from the secured remote not executed (injection path broken?)"
    assert base.gpio("A", 4) == ssr0, "SSR not restored"


def test_unsecured_remote_link_kicked(remote_away):
    """Re-review I-1: a link with the controller's address, %STREAM_OPEN%, but no %SECURED%/%ERR_SEC% -> after ~3 s the base sends
    B again and after another ~3 s K,1 (then auto-reconnect after 5 s). A link without an accepted peer must not hang forever."""
    base = remote_away
    proc, sr = la.start(12, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        base.inject_status(f"CONNECT,0,{REMOTE}")
        time.sleep(0.3)
        base.inject_status("STREAM_OPEN")  # t0: B, no reply (the module is not actually connected)
        time.sleep(8.0)
        accepted = base.read_bool("Bluetooth::_peerAccepted")
    finally:
        la.finish(proc, sr, 30)
        base.inject_status("DISCONNECT")  # firmware state back
    data, _ = la.uart_timed(sr, la.CH["base_ble_tx"], BAUD, 1e6)
    tx = bytes(b for _, b in data if 32 <= b < 127 or b == 13)
    assert not accepted, "peer accepted without %SECURED%"
    assert tx.count(b"B\r") == 2, f"expected B on STREAM_OPEN and one B again: {tx!r}"
    second_b = tx.index(b"B\r", tx.index(b"B\r") + 1)
    assert b"K,1\r" in tx[second_b:], f"unsecured link not kicked after the second B: {tx!r}"


def test_connect_while_scanning_kicked(remote_away):
    """%CONNECT% during the pairing scan (nobody is supposed to connect to us) -> K,1, the scan keeps running."""
    base = remote_away
    proc, sr = la.start(8, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        assert base.call("Bluetooth::startPairing()") == 0
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == dbg.PAIR_SCANNING, 2, what="SCANNING")
        time.sleep(0.5)
        base.inject_status(f"CONNECT,0,{STRANGER}")
        time.sleep(1.5)
        state = base.read_u8("Bluetooth::_pairingState")
        accepted = base.read_bool("Bluetooth::_peerAccepted")
    finally:
        la.finish(proc, sr, 30)
        base.inject_status("DISCONNECT")
        # pairing runs out by timeout (15 s), so that the fixture can restore the link to the controller
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") not in (1, 2, 3), 20, interval=0.5, what="pairing ended")
    tx = tx_text(sr)
    after_f = tx[tx.index(b"F\r"):]
    assert b"K,1\r" in after_f, f"connect during SCANNING not kicked: {tx!r}"
    assert state == dbg.PAIR_SCANNING, f"pairing state {state}"
    assert not accepted


SETTINGS_REMOTE_ADDR_OFFSET = 8  # settings::ramCopy.remoteAddr (after the 8 B of version 1 data, src/settings.cpp)


def test_no_reconnect_without_stored_remote(remote_away):
    """Empty controller address (upgrade from settings v1, ruling R32 b): the base does not connect to anything (no Z/C every
    5 s). The address is erased only in the RAM copy of the settings (flash unchanged) and restored at the end. Positive check: after
    the address is restored a "C" arrives within ~6 s. The "Sparuj ovladac" hint in the main menu title is checked by eye."""
    base = remote_away
    addr = base.sym("settings::ramCopy") + SETTINGS_REMOTE_ADDR_OFFSET
    saved = base.read(addr, 13)
    assert saved[:12].decode(errors="replace") == REMOTE, f"stored remote {saved!r}"
    proc, sr = la.start(20, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)  # the capture runs ~1 s before the erase
    try:
        base.write(addr, b"\0")
        time.sleep(12.0)
        base.write(addr, saved)
        time.sleep(7.0)
    finally:
        base.write(addr, saved)
        la.finish(proc, sr, 40)
    data, _ = la.uart_timed(sr, la.CH["base_ble_tx"], BAUD, 1e6)

    def text(t0, t1):
        return bytes(b for t, b in data if t0 <= t < t1)

    # 2.5 s: margin for the capture start and for a connection attempt started just before the erase.
    # The address is restored at ~13.0 s of Python time, but the capture (sigrok) starts later than the Python clock, so
    # the immediate reconnect after the restore lands at around 12.98 s of capture time. The negative window therefore ends 1 s earlier
    # (12.0) and the positive window starts from there; the negative window (9.5 s) still covers more than one 5 s connection interval.
    assert b"C\r" not in text(2.5, 12.0), f"reconnect attempt while no remote was stored: {text(2.5, 12.0)!r}"
    assert b"C\r" in text(12.0, 21.0), f"no reconnect after the address was restored: {text(12.0, 21.0)!r}"


def test_pairing_ignores_foreign_connect(remote_away):
    base = remote_away
    proc, sr = la.start(8, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        assert base.call("Bluetooth::startPairing()") == 0
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == dbg.PAIR_SCANNING, 2, what="SCANNING")
        time.sleep(0.5)
        base.inject_status(f"{REMOTE},0,MIDIControlRemote,,F5")  # remote advertisement -> X, C,0,<remote>
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == dbg.PAIR_CONNECTING, 2, what="CONNECTING")
        time.sleep(0.5)
        base.inject_status(f"CONNECT,0,{STRANGER}")  # somebody else connects
        time.sleep(1.5)
        state = base.read_u8("Bluetooth::_pairingState")
    finally:
        la.finish(proc, sr, 30)
        base.inject_status("DISCONNECT")
    tx = tx_text(sr)
    assert f"C,0,{REMOTE}\r".encode() in tx, tx
    after_connect = tx[tx.index(f"C,0,{REMOTE}\r".encode()):]
    assert b"B\r" not in after_connect, f"foreign %CONNECT% treated as the remote: {tx!r}"
    assert b"K,1\r" in after_connect, tx
    assert state == dbg.PAIR_FAILED, f"pairing state {state}"


def test_command_queued_during_exit_survives(base):
    """A command added to the queue while the state machine waits for END after '---' (EXIT_CMD) must be sent.
    The state is set via the probe at a safe point of the loop: job "V" has finished (_jobIndex = _jobCount = 1, EXIT_CMD,
    END has not arrived yet) and a "D" is added after it (_jobCount = 2) - exactly what queueCommand() does at that moment.
    The state machine's timestamp is old, so EXIT_TIMEOUT expires right after resuming."""
    jobs, count, index = base.sym("Bluetooth::_jobCommands"), base.sym("Bluetooth::_jobCount"), base.sym("Bluetooth::_jobIndex")
    state, saw_end = base.sym("Bluetooth::_engineState"), base.sym("Bluetooth::_sawEnd")
    proc, sr = la.start(5, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        with base.safe_halt():
            assert base.read(state, 1) == b"\0" and base.read(count, 1) == b"\0", "engine busy"
            base.write(jobs, b"V\0")
            base.write(jobs + 32, b"D\0")
            base.write(index, b"\x01")
            base.write(count, b"\x02")
            base.write(saw_end, b"\0")
            base.write(state, b"\x05")  # EngineState::EXIT_CMD
        time.sleep(2.0)
        left = base.read(count, 1)[0], base.read(state, 1)[0]
    finally:
        la.finish(proc, sr, 30)
    tx = tx_text(sr)
    assert b"D\r" in tx, f"command queued during EXIT_CMD was dropped: {tx!r}"
    assert left == (0, 0), f"engine not idle afterwards: count/state {left}"
