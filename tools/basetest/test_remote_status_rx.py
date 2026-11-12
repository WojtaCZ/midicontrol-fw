"""The remote's RX parser (midicontroller-fw src/logic/status_framer, host tests test_status_framer.cpp) on the bench.

Root cause 1 (FAULTS.md, bench 2026-11-12): a lost '%' left the old parser inside a "status" for good; every protocol frame
after it went into the status buffer, so no keepalive reached link and the base dropped the remote while BLE stayed up.
Root cause 2: the module sat in command mode while the firmware believed it was in data mode, so the base's frames came as
"%DATA:<handle>,<char>,<hex>%" and nothing was understood; the firmware now ends the link (K,1) and leaves command mode
(---) on the next link. Needs both probes and a remote paired with the bench base (see test_remote_guard.py)."""

import time

import pytest

from conftest import remote_alive, wait_until
from test_remote_guard import LINK_S, _anon, guard, inject_rx, linked_to_base, paired  # noqa: F401 (fixtures)

pytestmark = pytest.mark.hw

# statusframe::Framer (52 B): text_[48], len_, active_, resyncs_ (u16). statusframe::StuckCommandMode (8 B): lastMs_, sent_,
# exitDue_, recoveries_ (u16)
F_ACTIVE, F_RESYNCS, FRAMER_SIZE = 49, 50, 52
S_RECOVERIES, STUCK_SIZE = 6, 8


def framer(remote):
    addr, size = _anon(remote, "status")
    assert size == FRAMER_SIZE, f"statusframe::Framer is {size} B, the test expects {FRAMER_SIZE}: update the offsets"
    raw = remote.read(addr, FRAMER_SIZE)
    return dict(active=raw[F_ACTIVE], resyncs=int.from_bytes(raw[F_RESYNCS:F_RESYNCS + 2], "little"))


def recoveries(remote):
    addr, size = _anon(remote, "stuck")
    assert size == STUCK_SIZE, f"statusframe::StuckCommandMode is {size} B, the test expects {STUCK_SIZE}: update the offsets"
    return int.from_bytes(remote.read(addr + S_RECOVERIES, 2), "little")


def test_lost_closing_delimiter_resyncs(base, paired, guard):
    """A status whose closing '%' was lost: the next frame start (0x01, not printable) ends it, keepalives keep coming."""
    remote = paired
    before = framer(remote)["resyncs"]
    inject_rx(remote, b"%CONN_PARAM,0010,0000,00FA")  # no closer
    wait_until(lambda: framer(remote)["resyncs"] > before, 5.0, what="framer gave up the unterminated status")
    assert not framer(remote)["active"]
    time.sleep(8.0)  # > KEEPALIVE_LOSS: with the old parser the base dropped the remote here
    assert linked_to_base(base, remote, guard)


def test_lost_opening_delimiter_resyncs(base, paired, guard):
    """A status whose opening '%' was lost: its closing '%' opens a bogus status that the next frame ends."""
    remote = paired
    before = framer(remote)["resyncs"]
    inject_rx(remote, b"SECURED%")
    wait_until(lambda: framer(remote)["resyncs"] > before, 5.0, what="framer gave up the bogus status")
    assert not framer(remote)["active"]
    time.sleep(5.0)
    assert linked_to_base(base, remote, guard)


def test_data_status_recovers_the_link(base, paired, guard):
    """%DATA% while the firmware is in data mode = the module is stuck in command mode: K,1, and the base is accepted
    again on the next link (the K,1 sends no %DISCONNECT%, see test_own_disconnect_lets_the_base_back_in)."""
    remote = paired
    before = recoveries(remote)
    inject_rx(remote, b"%DATA:0079,000A,0106020360008004%")
    wait_until(lambda: recoveries(remote) > before, 3.0, what="stuck command mode noticed")
    wait_until(lambda: not remote_alive(base), 10.0, what="K,1 ended the link")
    wait_until(lambda: linked_to_base(base, remote, guard), LINK_S, what="base back after the recovery")
    # back in data mode (a short command-mode session, the RSSI poll or the "---" after the relink, may be running)
    mode = remote.sym("Bluetooth::(anonymous namespace)::_mode")
    wait_until(lambda: remote.read(mode, 1)[0] == 1, 3.0, interval=0.05, what="firmware back in Mode::DATA")
