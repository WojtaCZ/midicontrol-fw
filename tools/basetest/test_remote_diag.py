"""Diagnostika remote pres base: DIAG_GET (0x80) -> ACK + DIAG_RESP (0x81), kruh 32 zaznamu v RAM (Task 6D-1).
Predpoklad: base a remote jsou sparovane (test_pairing.py), remote bezi nove firmware s diag."""

import time

import pytest

import la
import proto
from conftest import remote_alive, wait_until
from proto import MT, NAK, Packet

ADDR_PC, ADDR_BASE, ADDR_REMOTE = proto.ADDR_PC, proto.ADDR_BASE, proto.ADDR_REMOTE
BAUD = 115200
CODE_BOOT = 1
CODE_RECORDS_LOST = 16
LEVEL_INFO = 3


def get(link, dst, cursor, timeout=2.0):
    """DIAG_GET -> (reply ACK/NAK, DIAG_RESP | None). DIAG_RESP chodi navic po ACK."""
    pid = link.send(Packet(ADDR_PC, dst, 0, MT.DIAG_GET, proto.diag_encode_get(cursor)))
    reply = link.recv(timeout, lambda p: p.src == dst and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    assert reply is not None, "no ACK/NAK for DIAG_GET"
    if reply.mtype != MT.ACK:
        return reply, None
    resp = link.recv(timeout, lambda p: p.src == dst and p.dst == ADDR_PC and p.mtype == MT.DIAG_RESP)
    return reply, resp


def decode(resp):
    return proto.diag_decode_resp(resp.payload)


@pytest.fixture(scope="module")
def paired(base):
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="BLE stream open (pair first: test_pairing.py)")
    wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
    return base


def test_diag_get_boot_record(paired, link):
    link.drain()
    reply, resp = get(link, ADDR_REMOTE, 0)
    assert reply.mtype == MT.ACK, reply
    assert resp is not None, "no DIAG_RESP after ACK"
    boot_id, nxt, recs = decode(resp)
    assert len(recs) >= 1 and len(recs) <= proto.DIAG_MAX_RECORDS
    first = recs[0]
    if first[0] != 0:
        pytest.skip(f"ring wrapped (first seq {first[0]}): reset the remote to see its BOOT record")
    seq, code, level, uptime, arg = first
    assert code == CODE_BOOT and level == LEVEL_INFO, first
    print(f"boot_id {boot_id:#06x} next {nxt} first {first} reset cause {arg:#04x}")


def test_diag_cursor_paging(paired, link, remote_probe):
    remote = remote_probe
    link.drain()
    _, resp = get(link, ADDR_REMOTE, 0xFFFF)  # kurzor za koncem: n = 0, next = head
    boot_id, head0, recs = decode(resp)
    assert recs == [], recs
    def head_now():
        try:  # hned po otevreni streamu remote jeste nemusi odpovidat
            _, r = get(link, ADDR_REMOTE, 0xFFFF, timeout=1.0)
            return decode(r)[1] if r else -1
        except AssertionError:
            return -1

    cycles, head = 0, head0
    while head - head0 < 10 and cycles < 5:
        remote.hold_ble_reset(True)
        wait_until(lambda: not paired.read_bool("Bluetooth::_streamOpen"), 10, what="base sees disconnect")
        remote.hold_ble_reset(False)
        wait_until(lambda: paired.read_bool("Bluetooth::_streamOpen"), 15, what="reconnected")
        cycles += 1
        # kazdy cyklus: BLE_DISCONNECT, BLE_CONNECT, BLE_SECURED
        wait_until(lambda: head_now() >= head + 3, 15, interval=0.5, what="records of the reset cycle")
        head = head_now()
    assert head - head0 >= 10, f"only {head - head0} records after {cycles} reset cycles"

    cur, seen, pages = head0, [], 0
    while cur < head:
        _, resp = get(link, ADDR_REMOTE, cur)
        bid, nxt, recs = decode(resp)
        assert bid == boot_id, "boot_id changed without a remote reset"
        assert 0 < len(recs) <= proto.DIAG_MAX_RECORDS
        seen += [r[0] for r in recs]
        assert nxt == recs[-1][0] + 1
        cur, pages = nxt, pages + 1
    assert seen == list(range(head0, head)), seen
    assert pages >= 2
    _, resp = get(link, ADDR_REMOTE, cur)
    assert decode(resp)[2] == [], "records past the head"
    print(f"{cycles} reset cycles, records {head0}..{head - 1} in {pages} pages")


def test_diag_get_to_base_nak(paired, link):
    link.drain()
    pid = link.send(Packet(ADDR_PC, ADDR_BASE, 0, MT.DIAG_GET, proto.diag_encode_get(0)))
    r = link.recv(1.5, lambda p: p.src == ADDR_BASE and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    assert r is not None and r.mtype == MT.NAK and r.payload == bytes([pid, NAK.UNKNOWN_MSG]), r


def test_diag_traffic_budget(paired, link):
    """60 s, DIAG_GET kazdych 10 s: nejvyse 6 DIAG_GET a 6 DIAG_RESP na BLE UART remote, kazdy <= 120 B."""
    link.drain()
    proc, sr = la.start(64, channels="D2,D3,D6,D7", samplerate="1M")
    time.sleep(1.5)
    try:
        t0 = time.monotonic()
        cursor = 0
        for i in range(6):
            reply, resp = get(link, ADDR_REMOTE, cursor)
            assert reply.mtype == MT.ACK and resp is not None, (i, reply)
            cursor = decode(resp)[1]
            wait = t0 + 10 * (i + 1) - time.monotonic()
            if i < 5 and wait > 0:
                time.sleep(wait)
        time.sleep(max(0.0, t0 + 60 - time.monotonic()))
    finally:
        la.finish(proc, sr, 90)
    rx = proto.FrameParser().feed(la.uart(sr, la.CH["rem_ble_rx"], BAUD))
    tx = proto.FrameParser().feed(la.uart(sr, la.CH["rem_ble_tx"], BAUD))
    gets = [p for p in rx if p.mtype == MT.DIAG_GET]
    resps = [p for p in tx if p.mtype == MT.DIAG_RESP]
    sizes = [len(proto.encode_packet(p)) for p in gets + resps]
    print(f"DIAG_GET {len(gets)}, DIAG_RESP {len(resps)}, wire sizes {sizes}")
    assert 1 <= len(gets) <= 6 and 1 <= len(resps) <= 6
    assert max(sizes) <= 120, sizes
