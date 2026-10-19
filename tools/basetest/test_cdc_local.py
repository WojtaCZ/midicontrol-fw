"""Base jako samostatny uzel pres USB-CDC: pakety adresovane Base se zpracuji lokalne."""

import re

import pytest

import proto
from proto import MT, NAK, Packet

ADDR_PC, ADDR_BASE = proto.ADDR_PC, proto.ADDR_BASE

# GUI::mainItems[2] (checkbox napajeni): velikost ui::MenuItem a offset checked_ z DWARF (build s -g, viz README). Bez DWARF nejde pole struktury cist jinak.
MENUITEM_SIZE, MENUITEM_CHECKED = 24, 0x14


@pytest.fixture
def quiet(link):
    """Bez pozadovych keepalive (nepocitaji se do ACK ani ticha); na konci se obnovi."""
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
    return base.read(base.sym("GUI::mainItems") + 2 * MENUITEM_SIZE + MENUITEM_CHECKED, 1)[0]


def test_keepalive_ack(quiet):
    quiet.send_msg(MT.KEEPALIVE)
    r = quiet.recv(1.0, lambda p: p.mtype == MT.KEEPALIVE_ACK)
    assert r is not None and r.src == ADDR_BASE and r.dst == ADDR_PC


def test_get_status(quiet, base):
    before = base.u("Bluetooth::_connected", 1)
    pid = quiet.send_msg(MT.GET_STATUS)
    ack = quiet.recv(1.0, is_reply(MT.ACK, pid))
    resp = quiet.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.STATUS_RESP)
    after = base.u("Bluetooth::_connected", 1)
    assert ack is not None and resp is not None
    assert len(resp.payload) == 4
    ble, current, display, pc_alive = resp.payload
    assert pc_alive == 1
    assert ble in (before, after)
    # STATUS_RESP se potvrzuje, jinak by ho base 3x opakovala a rusila by dalsi testy
    quiet.send(Packet(ADDR_PC, ADDR_BASE, 0, MT.ACK, bytes([resp.pid])))
    assert quiet.expect_silence(0.7) == []


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
    # Duplicitni pid se jen znovu potvrdi. Dukaz "provedeno jednou": druhy rámec ma STEJNY pid, ale JINY
    # obsah ([0] misto [1]). Kdyby ho base provedla, pin PA4 by spadl na 0; dedup ho nechá na 1.
    # (Rozdilny pid mezi nimi by dedup neuplatnil, proto pid dodavame sami pres send_raw.)
    quiet.send_msg(MT.SET_CURRENT, b"\x00")  # znamy vychozi stav, jiny pid nez DUP
    assert quiet.recv(1.0, is_reply(MT.ACK)) is not None
    assert base.gpio("A", 4) == 0
    DUP = 0xC3
    quiet.send_raw(proto.encode(ADDR_PC, ADDR_BASE, DUP, MT.SET_CURRENT, b"\x01"))
    assert quiet.recv(1.0, is_reply(MT.ACK, DUP)) is not None
    assert base.gpio("A", 4) == 1
    quiet.send_raw(proto.encode(ADDR_PC, ADDR_BASE, DUP, MT.SET_CURRENT, b"\x00"))
    assert quiet.recv(1.0, is_reply(MT.ACK, DUP)) is not None  # druhy ACK
    assert base.gpio("A", 4) == 1  # nebylo provedeno
    assert quiet.expect_silence(0.3) == []
    quiet.send_msg(MT.SET_CURRENT, b"\x00")
    assert quiet.recv(1.0, is_reply(MT.ACK)) is not None


def test_wrong_dest_ignored(quiet):
    quiet.send_msg(MT.KEEPALIVE, dst=0x09)
    assert quiet.expect_silence(0.5) == []


def test_debug_log_not_truncated(base):
    """Log v no-init RAM drzi uplne zpravy: znamy radek musi byt v logu doslovne a cely. Deterministicke radky:
    "ble: cmd failed: C" (reconnect "C" bez bondu -> Err) a "ble: pairing timeout" (test_pairing.py). Od b52c8f7
    "C" s bondem odpovida "Trying" a neloguje se, takze se spojenym remote radek nemusi vzniknout. Kazdy radek ma
    format "[ssssss.mmm] L tag: text"; ring ma jen 1536 B, takze banner uz po jinych testech v RAM byt nemusi."""
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
