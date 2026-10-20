"""Diagnostika remote pres base: DIAG_GET (0x80) -> ACK + DIAG_RESP (0x81), kruh 32 zaznamu v RAM (Task 6D-1).
Predpoklad: base a remote jsou sparovane (test_pairing.py), remote bezi nove firmware s diag."""

import time

import pytest

import proto
from conftest import remote_alive, wait_until
from proto import MT, NAK, Packet

ADDR_PC, ADDR_BASE, ADDR_REMOTE = proto.ADDR_PC, proto.ADDR_BASE, proto.ADDR_REMOTE
CODE_BOOT = 1
CODE_RECORDS_LOST = 16
LEVEL_INFO = 3


def get(link, dst, cursor, timeout=2.0):
    """DIAG_GET -> (reply ACK/NAK, DIAG_RESP | None). DIAG_RESP chodi navic po ACK."""
    # pozdni DIAG_RESP z drivejsiho pozadavku (po timeoutu) by se jinak spletla s odpovedi na tento
    link.drain()
    pid = link.send(Packet(ADDR_PC, dst, 0, MT.DIAG_GET, proto.diag_encode_get(cursor)))
    reply = link.recv(timeout, lambda p: p.src == dst and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    assert reply is not None, "no ACK/NAK for DIAG_GET"
    if reply.mtype != MT.ACK:
        return reply, None
    resp = link.recv(timeout, lambda p: p.src == dst and p.dst == ADDR_PC and p.mtype == MT.DIAG_RESP)
    if resp is not None:  # DIAG_RESP se potvrzuje (PROTOCOL.md), jinak ho remote opakuje
        link.send(Packet(ADDR_PC, dst, 0, MT.ACK, bytes([resp.pid])))
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


def test_diag_traffic_budget(paired, link, remote_probe):
    """60 s, DIAG_GET kazdych 10 s s plnou davkou (8 zaznamu): presne jeden ACK a jedna DIAG_RESP na GET, nic
    navic (ani opakovani, ani neziadane), payload <= 101 B, ramec na drate <= 2x nezeskapovany + 2 B.
    Pocita se end-to-end pres `link` (bez ztrat); logicky analyzator se nepouziva, jeho dekodovani 64 s zaznamu
    ztracelo ramce a mez 120 B zavisi na datech (escapuji se jen 0x01/0x04/0x1B/'$'/'%')."""
    remote = remote_probe

    def head_now():
        try:
            _, r = get(link, ADDR_REMOTE, 0xFFFF, timeout=1.0)
            return decode(r)[1] if r else -1
        except AssertionError:
            return -1

    while head_now() < 8:  # zasoba >= 8 zaznamu, aby se zkusila plna davka
        remote.hold_ble_reset(True)
        wait_until(lambda: not paired.read_bool("Bluetooth::_streamOpen"), 10, what="base sees disconnect")
        remote.hold_ble_reset(False)
        wait_until(lambda: paired.read_bool("Bluetooth::_streamOpen"), 15, what="reconnected")
        time.sleep(3.0)
    time.sleep(2.0)
    link.drain()

    sent_at, sizes, counts = [], [], []
    cursor = 0
    t0 = time.monotonic()
    for i in range(6):
        sent_at.append(time.monotonic())
        reply, resp = get(link, ADDR_REMOTE, cursor)
        assert reply.mtype == MT.ACK and resp is not None, (i, reply)
        assert len(resp.payload) <= proto.DIAG_RESP_MAX_PAYLOAD, len(resp.payload)
        unescaped = len(proto.raw_body(resp.src, resp.dst, resp.pid, resp.mtype, resp.payload))
        wire = len(proto.encode_packet(resp))
        assert wire <= 2 * unescaped + 2, (wire, unescaped)
        sizes.append(wire)
        counts.append(len(decode(resp)[2]))
        if i == 0:
            assert counts[0] == proto.DIAG_MAX_RECORDS, f"first page has {counts[0]} records, wanted the full 8"
        cursor = decode(resp)[1]
        if i < 5:
            time.sleep(max(0.0, t0 + 10 * (i + 1) - time.monotonic()))
            # mezitim nic nepriteklo: zadny duplikat DIAG_RESP (retry) ani neziadany ramec od remote
            extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype in (MT.DIAG_RESP, MT.ACK, MT.NAK)]
            assert extra == [], f"unsolicited/duplicate frames from remote after GET {i}: {extra}"
    gaps = [b - a for a, b in zip(sent_at, sent_at[1:])]
    assert all(g >= 9.5 for g in gaps), gaps
    time.sleep(2.0)
    extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype in (MT.DIAG_RESP, MT.ACK, MT.NAK)]
    assert extra == [], extra
    print(f"6 GET -> 6 ACK + 6 DIAG_RESP, records per page {counts}, wire sizes {sizes}, gaps {[round(g, 1) for g in gaps]}")
