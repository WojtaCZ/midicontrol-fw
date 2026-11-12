"""The base as a standalone node over USB-CDC: packets addressed to the Base are handled locally."""

import re

import pytest

import proto
from proto import MT, NAK, Packet

ADDR_PC, ADDR_BASE = proto.ADDR_PC, proto.ADDR_BASE

# GUI::settingsItems[2] (power checkbox, moved from mainItems into the settings screen): the size of ui::MenuItem and the offset of checked_ from DWARF (build with -g, see README). Without DWARF a struct field cannot be read any other way.
MENUITEM_SIZE, MENUITEM_CHECKED = 24, 0x14


@pytest.fixture
def quiet(link):
    """Without background keepalives (they count neither toward ACKs nor toward silence); restored at the end."""
    link.pause_keepalive(True)
    link.drain()
    try:
        yield link
    finally:
        link.pause_keepalive(False)


def is_reply(mtype, pid=None):
    def pred(p):
        return p.src == ADDR_BASE and p.mtype == mtype and (pid is None or (p.payload[:1] == bytes([pid])))
    return pred


def checkbox_power(base) -> int:
    return base.read(base.sym("GUI::settingsItems") + 2 * MENUITEM_SIZE + MENUITEM_CHECKED, 1)[0]


def test_keepalive_ack(quiet):
    quiet.send_msg(MT.KEEPALIVE)
    r = quiet.recv(1.0, lambda p: p.mtype == MT.KEEPALIVE_ACK)
    assert r is not None and r.src == ADDR_BASE and r.dst == ADDR_PC


def link_usable(base) -> int:
    return int(all(base.u(n, 1) for n in ("Bluetooth::_connected", "Bluetooth::_streamOpen", "Bluetooth::_peerAccepted")))


def get_status(quiet):
    """GET_STATUS -> (ack, STATUS_RESP); the response is acknowledged, otherwise the base would repeat it 3x and disturb
    the other tests."""
    pid = quiet.send_msg(MT.GET_STATUS)
    ack = quiet.recv(1.0, is_reply(MT.ACK, pid))
    resp = quiet.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.STATUS_RESP)
    if resp is not None:
        quiet.send(Packet(ADDR_PC, ADDR_BASE, 0, MT.ACK, bytes([resp.pid])))
    return ack, resp


def test_get_status(quiet, base):
    before = link_usable(base)
    ack, resp = get_status(quiet)
    after = link_usable(base)
    assert ack is not None and resp is not None
    assert len(resp.payload) == 4
    ble, current, display, pc_alive = resp.payload
    assert pc_alive == 1
    assert ble in (before, after)
    assert quiet.expect_silence(0.7) == []


def test_status_ble_only_when_secured(quiet, base):
    """SYS-LINK-07 (gap 10): bleConnected is 1 only for a usable (secured) link. The probe clears _peerAccepted for one
    request (connected + stream open, not secured: the state between %STREAM_OPEN% and %SECURED%), then restores it."""
    from conftest import remote_alive, wait_until
    wait_until(lambda: link_usable(base) and remote_alive(base), 15.0, what="secured remote link")
    assert get_status(quiet)[1].payload[0] == 1
    addr = base.sym("Bluetooth::_peerAccepted")
    base.write(addr, b"\x00")
    try:
        assert base.u("Bluetooth::_connected", 1) and base.u("Bluetooth::_streamOpen", 1)
        ack, resp = get_status(quiet)
        assert ack is not None and resp is not None
        assert resp.payload[0] == 0, f"connected but not secured reported as connected: {resp.payload.hex()}"
    finally:
        base.write(addr, b"\x01")
    assert get_status(quiet)[1].payload[0] == 1


def test_set_current_on_off(quiet, base):
    for value in (1, 0, 1, 0):
        pid = quiet.send_msg(MT.SET_CURRENT, bytes([value]))
        assert quiet.recv(1.0, is_reply(MT.ACK, pid)) is not None
        assert base.gpio("A", 4) == value
        assert checkbox_power(base) == value  # GUI::syncPowerCheckbox


def test_set_current_bad_payload(quiet):
    pid = quiet.send_msg(MT.SET_CURRENT, b"")
    nak = quiet.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.NAK)
    assert nak is not None and nak.payload == bytes([pid, NAK.INVALID_PAYLOAD])


def test_unknown_type_nak(quiet):
    pid = quiet.send_msg(0x7E)
    nak = quiet.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.NAK)
    assert nak is not None and nak.payload == bytes([pid, NAK.UNKNOWN_MSG])


def test_duplicate_pid_executed_once(quiet, base):
    # A duplicate pid is only acknowledged again. Proof of "executed once": the second frame has the SAME pid but DIFFERENT
    # content ([0] instead of [1]). If the base executed it, pin PA4 would drop to 0; dedup leaves it at 1.
    # (A different pid between them would not trigger dedup, hence we supply the pid ourselves via send_raw.)
    quiet.send_msg(MT.SET_CURRENT, b"\x00")  # known initial state, a pid other than DUP
    assert quiet.recv(1.0, is_reply(MT.ACK)) is not None
    assert base.gpio("A", 4) == 0
    DUP = 0xC3
    quiet.send_raw(proto.encode(ADDR_PC, ADDR_BASE, DUP, MT.SET_CURRENT, b"\x01"))
    assert quiet.recv(1.0, is_reply(MT.ACK, DUP)) is not None
    assert base.gpio("A", 4) == 1
    quiet.send_raw(proto.encode(ADDR_PC, ADDR_BASE, DUP, MT.SET_CURRENT, b"\x00"))
    assert quiet.recv(1.0, is_reply(MT.ACK, DUP)) is not None  # second ACK
    assert base.gpio("A", 4) == 1  # was not executed
    assert quiet.expect_silence(0.3) == []
    quiet.send_msg(MT.SET_CURRENT, b"\x00")
    assert quiet.recv(1.0, is_reply(MT.ACK)) is not None


def test_wrong_dest_ignored(quiet):
    quiet.send_msg(MT.KEEPALIVE, dst=0x09)
    assert quiet.expect_silence(0.5) == []


def test_debug_log_not_truncated(base):
    """The log in no-init RAM holds complete messages: a known line must be in the log verbatim and whole. Deterministic lines:
    "ble: cmd failed: C" (reconnect "C" without a bond -> Err) and "ble: pairing timeout" (test_pairing.py). Since b52c8f7
    "C" with a bond answers "Trying" and is not logged, so with a linked remote the line need not appear. Every line has the
    format "[ssssss.mmm] L tag: text"; the ring is only 1536 B, so the banner may no longer be in RAM after other tests."""
    lines = base.debug_log()
    assert lines
    bad = [l for l in lines if not re.match(r"^\[ *\d+\.\d{3}\] [EWID] [a-z]{1,6}: .{0,120}~?$", l)]
    assert not bad, bad
    msgs = [m.group(1) for m in (re.match(r"^\[ *\d+\.\d{3}\] W ble: (.*)$", l) for l in lines) if m]
    if not any(m == "cmd failed: C" or m.startswith("pairing timeout") for m in msgs):
        if base.u("Bluetooth::_connected", 1):
            pytest.skip("remote connected and no known line in the log yet (run after test_pairing.py)")
        assert False, lines


def test_stray_is_empty(link):
    assert link.stray == b""
