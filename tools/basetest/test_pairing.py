"""First pairing of the base with the remote (menu "Sparovat ovladac" -> Bluetooth::startPairing): scan, connecting only to
MIDIController, bond (Just Works: base and remote SA,2 NoInputNoOutput, see CLAUDE.md), reconnection after a
base reset and a timeout without the remote. The recording of both RN4870 UARTs during pairing goes to PAIRING_LOG (default: tmp)."""

import os
import re
import tempfile
import time

import pytest

import dbg
import la
from conftest import remote_alive, wait_until

REMOTE_ADDR = "D88039FE5996"
SETTINGS_ADDR = 0x0801F800
BAUD = 115200
LOG = os.environ.get("PAIRING_LOG", os.path.join(tempfile.gettempdir(), "basetest-pairing.log"))


def disconnect_and_forget(base):
    """The base forgets all bonds (U,Z) and disconnects if connected. Order U,Z -> K,1: an auto-reconnect ("C" to the
    last bonded device) right after the disconnect would succeed if the bond still existed."""
    base.call('Bluetooth::queueCommand("U,Z")')
    if base.read_bool("Bluetooth::_connected"):
        base.call('Bluetooth::queueCommand("K,1")')
    wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 10, what="base disconnected")


def status_lines(data: bytes) -> list[str]:
    return re.findall(r"%([^%\r\n]{1,100})%", data.decode("latin1"))


def pair(base, timeout=20):
    base.call("Bluetooth::startPairing()")
    wait_until(lambda: base.read_u8("Bluetooth::_pairingState") in (dbg.PAIR_DONE, dbg.PAIR_FAILED), timeout, what="pairing finished")
    return base.read_u8("Bluetooth::_pairingState")


def write_log(sr, title):
    """Status messages of both RN4870s and the base commands into PAIRING_LOG; returns (base_rx, base_tx, remote_rx)."""
    base_rx = status_lines(la.uart(sr, la.CH["base_ble_rx"], BAUD))
    base_tx = la.uart(sr, la.CH["base_ble_tx"], BAUD)
    rem_rx = status_lines(la.uart(sr, la.CH["rem_ble_rx"], BAUD))
    rem_tx = la.uart(sr, la.CH["rem_ble_tx"], BAUD)
    with open(LOG, "a") as f:
        f.write(f"=== {title} {time.strftime('%H:%M:%S')}\n")
        f.write(f"base MCU->RN4870: {base_tx!r}\nbase RN4870 status: {base_rx}\n")
        f.write(f"remote RN4870 status: {rem_rx}\nremote MCU->RN4870 (frames dropped): {bytes(b for b in rem_tx if 0x20 <= b < 0x7f or b == 13)!r}\n")
    return base_rx, base_tx, rem_rx


def test_pairing_finds_remote_and_bonds(base, remote):
    disconnect_and_forget(base)
    proc, sr = la.start(25, channels="D2,D3,D6,D7", samplerate="1M")
    time.sleep(1.0)
    try:
        state = pair(base)
        assert state == dbg.PAIR_DONE, f"pairing state {state}"
        assert base.read_bool("Bluetooth::_bonded")
        # the link works: the base keepalive reaches the remote and the remote answers
        wait_until(lambda: remote_alive(base), 6, interval=0.5, what="remote alive (router::isRemoteAlive)")
    finally:
        la.finish(proc, sr, 60)
        base_rx, base_tx, rem_rx = write_log(sr, "test_pairing_finds_remote_and_bonds")

    # base: connected only to the remote, stream open, secured/bonded
    connects = [s for s in base_rx if s.startswith("CONNECT,")]
    assert connects == [f"CONNECT,0,{REMOTE_ADDR}"], base_rx
    assert "STREAM_OPEN" in base_rx, base_rx
    assert "SECURED" in base_rx or "BONDED" in base_rx, base_rx
    assert "ERR_SEC" not in base_rx, base_rx
    assert f"C,0,{REMOTE_ADDR}".encode() in base_tx
    # the controller address stored permanently (settings v2 at 0x0801F800: magic, version, LED, MIDI, pad, remoteAddr[13])
    assert base.read(SETTINGS_ADDR + 8, 13).split(b"\0")[0] == REMOTE_ADDR.encode()
    # remote: the same from the other side
    assert any(s.startswith("CONNECT,") for s in rem_rx), rem_rx
    assert "STREAM_OPEN" in rem_rx, rem_rx
    assert "SECURED" in rem_rx or "BONDED" in rem_rx, rem_rx
    assert "ERR_SEC" not in rem_rx, rem_rx


def test_reconnect_after_base_reset_uses_bond(base, remote, link):
    assert base.read_bool("Bluetooth::_bonded"), "needs the bond from test_pairing_finds_remote_and_bonds"
    link.detach()
    base.reset()
    link.attach()
    time.sleep(0.5)
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="stream open after reset")  # "C" to the bonded remote
    # the controller address survived the reset (settings in flash) -> B after %STREAM_OPEN% -> %SECURED%
    wait_until(lambda: base.read_bool("Bluetooth::_bonded"), 3, what="reconnected link secured (%SECURED%)")
    wait_until(lambda: remote_alive(base), 6, interval=0.5, what="router::isRemoteAlive()")


def test_pairing_timeout_no_remote(base, remote_probe):
    remote = remote_probe
    remote.hold_ble_reset(True)  # remote RN4870 in reset (PB8)
    try:
        disconnect_and_forget(base)
        t0 = time.monotonic()
        state = pair(base, timeout=20)
        took = time.monotonic() - t0
        assert state == dbg.PAIR_FAILED, f"pairing state {state}"
        assert 14 < took < 20, f"FAILED after {took:.1f} s (PAIRING_TIMEOUT 15 s)"
        # surrounding devices (phones, PC) must not connect in place of the remote
        assert not base.read_bool("Bluetooth::_connected")
    finally:
        remote.hold_ble_reset(False)
    # restore the bond for the other tests (after the reset is released the remote starts advertising again)
    time.sleep(3)
    proc, sr = la.start(25, channels="D2,D3,D6,D7", samplerate="1M")
    time.sleep(1.0)
    try:
        state = pair(base, timeout=20)
    finally:
        la.finish(proc, sr, 60)
        write_log(sr, "test_pairing_timeout_no_remote: re-pair")
    assert state == dbg.PAIR_DONE, f"re-pairing state {state}"
    wait_until(lambda: remote_alive(base), 6, interval=0.5, what="remote alive after re-pairing")
