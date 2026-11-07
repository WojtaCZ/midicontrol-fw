"""RNBD350: a peer that vanishes (remote module reset or powered off) is reported as %ERR_CONN%, not %DISCONNECT%
(bench 2026-11-07). The base must drop the link state within a few seconds and reconnect when the remote is back."""
import time

import pytest

from conftest import remote_alive, wait_until

pytestmark = pytest.mark.hw


def test_vanishing_peer_is_detected_and_link_recovers(base, remote_probe):
    wait_until(lambda: remote_alive(base), 30, interval=0.5, what="remote alive before the test")
    remote_probe.hold_ble_reset(True)
    try:
        wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 6, what="base noticed the lost link (ERR_CONN)")
        assert not base.read_bool("Bluetooth::_streamOpen")
        assert not remote_alive(base)
    finally:
        remote_probe.hold_ble_reset(False)
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 20, what="base reconnected")
    wait_until(lambda: remote_alive(base), 10, interval=0.5, what="remote alive again")
