"""First pairing of the base with the remote (menu "Sparovat ovladac" -> Bluetooth::startPairing): scan, connecting only to
MIDIControlRemote, bond (Just Works: base and remote SA,2 NoInputNoOutput, see CLAUDE.md), reconnection after a
base reset and a timeout without the remote.
No logic analyser (owner 2026-11-22): asserts use only state the STM32s read."""

import time

import pytest

import dbg
from conftest import remote_alive, wait_until

REMOTE_ADDR = "40843204625F"
SETTINGS_ADDR = 0x0801F800


def disconnect_and_forget(base):
    """The base forgets all bonds (U,Z) and disconnects if connected. Order U,Z -> K,1: an auto-reconnect ("C" to the
    last bonded device) right after the disconnect would succeed if the bond still existed."""
    base.call('Bluetooth::queueCommand("U,Z")')
    if base.read_bool("Bluetooth::_connected"):
        base.call('Bluetooth::queueCommand("K,1")')
    wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 10, what="base disconnected")


def remote_waits_for_base(remote) -> bool:
    """The remote's noBaseScreen shows "Cekam na zakladnu" (GUI::noBaseComment, a const char*)."""
    if remote.screen_name() != "noBaseScreen":
        return False
    ptr = int.from_bytes(remote.read(remote.sym("GUI::noBaseComment"), 4), "little")
    return remote.read(ptr, 32).split(b"\0")[0] == b"Cekam na zakladnu"


def pair(base, timeout=20):
    base.call("Bluetooth::startPairing()")
    wait_until(lambda: base.read_u8("Bluetooth::_pairingState") in (dbg.PAIR_DONE, dbg.PAIR_FAILED), timeout, what="pairing finished")
    return base.read_u8("Bluetooth::_pairingState")


def test_pairing_finds_remote_and_bonds(base, remote):
    disconnect_and_forget(base)
    state = pair(base)
    assert state == dbg.PAIR_DONE, f"pairing state {state}"
    assert base.read_bool("Bluetooth::_bonded")
    # bond + stream open take ~6 s after PAIR_DONE (bench 2026-11-22)
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="stream open")
    # the link works: the base keepalive reaches the remote and the remote answers
    wait_until(lambda: remote_alive(base), 15, interval=0.5, what="remote alive (router::isRemoteAlive)")
    # the controller address stored permanently (settings v2 at 0x0801F800: magic, version, LED, MIDI, pad, remoteAddr[13])
    assert base.read(SETTINGS_ADDR + 8, 13).split(b"\0")[0] == REMOTE_ADDR.encode()
    # remote: sees the base (with the daemon stopped it may still show "Cekam na PC", which is fine)
    wait_until(lambda: not remote_waits_for_base(remote), 10, what="remote stops waiting for the base")


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
    state = pair(base, timeout=20)
    assert state == dbg.PAIR_DONE, f"re-pairing state {state}"
    wait_until(lambda: remote_alive(base), 15, interval=0.5, what="remote alive after re-pairing")
