"""Bez hardwaru: fwclient (FW_UPDATE_* klient harnessu) a vyroba vadnych obrazu pro testy bootloaderu."""

import os
import struct
import sys
import zlib

import pytest

import fwclient
import proto
from proto import MT, Packet

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "lib", "bootloader", "tools"))
import fw_trailer  # noqa: E402


def good_image(board=0x02, n=1000):
    app = bytes((i * 7) & 0xFF for i in range(n))
    return fw_trailer.build(app, board, 0x000100)


def test_make_image_resigned_is_valid():
    img = fwclient.make_image(good_image(), board=0x02)
    assert fw_trailer.check(img, 0x02) is None


def test_make_image_board_mismatch():
    img = fwclient.make_image(good_image(), board=0x03)
    assert fw_trailer.check(img, 0x02).startswith("board 0x03")
    assert fw_trailer.check(img, 0x03) is None


def test_make_image_corrupt_trailer():
    assert fw_trailer.check(fwclient.make_image(good_image(), corrupt_trailer=True), 0x02) == "corrupt trailer"


def test_make_image_corrupt_body_keeps_trailer():
    img = fwclient.make_image(good_image(), corrupt_body=True)
    assert fw_trailer.check(img, 0x02) == "image CRC mismatch"


def test_make_image_truncated():
    img = fwclient.make_image(good_image(), truncate=100)
    assert len(img) == len(good_image()) - 100


def test_start_and_data_payloads():
    assert fwclient.start_payload(0x02, 0x1234, 0xDEADBEEF) == bytes([0x02]) + struct.pack("<II", 0x1234, 0xDEADBEEF)
    assert fwclient.data_payload(0x78, b"abc") == struct.pack("<I", 0x78) + b"abc"
    assert fwclient.CHUNK == 120


class FakeLink:
    """Odpovida jako FwReceiver: ACK na kazdy paket, STATUS po START (READY) a END (CRC_OK nebo zadany)."""

    def __init__(self, nak_type=None, end_status=0x02):
        self.sent = []
        self.q = []
        self.nak_type = nak_type
        self.end_status = end_status
        self._pid = 0

    def send(self, pkt):
        self._pid = (self._pid + 1) & 0xFF
        self.sent.append(pkt)
        kind = MT.NAK if pkt.mtype == self.nak_type else MT.ACK
        self.q.append(Packet(pkt.dst, pkt.src, 0, kind, bytes([self._pid])))
        if kind == MT.ACK and pkt.mtype == MT.FW_UPDATE_START:
            self.q.append(Packet(pkt.dst, pkt.src, 0, MT.FW_UPDATE_STATUS, bytes([0x00])))
        if pkt.mtype == MT.FW_UPDATE_END:
            self.q.append(Packet(pkt.dst, pkt.src, 0, MT.FW_UPDATE_STATUS, bytes([self.end_status])))
        return self._pid

    def recv(self, timeout, pred=None):
        for i, p in enumerate(self.q):
            if pred is None or pred(p):
                return self.q.pop(i)
        return None

    def drain(self):
        out, self.q = self.q, []
        return out


def test_send_image_happy_path():
    img = good_image(n=1000)
    L = FakeLink()
    assert fwclient.Client(L, proto.ADDR_BASE).send_image(img) == "ok"
    kinds = [p.mtype for p in L.sent]
    assert kinds[0] == MT.FW_UPDATE_START and kinds[-1] == MT.FW_UPDATE_END
    data = [p for p in L.sent if p.mtype == MT.FW_UPDATE_DATA]
    assert b"".join(p.payload[4:] for p in data) == img
    assert all(len(p.payload) - 4 <= 120 for p in data)
    assert L.sent[0].payload == fwclient.start_payload(0x02, len(img), zlib.crc32(img))


def test_send_image_stop_after():
    img = good_image(n=1000)
    L = FakeLink()
    assert fwclient.Client(L, proto.ADDR_BASE).send_image(img, stop_after=240) == "stopped"
    data = [p for p in L.sent if p.mtype == MT.FW_UPDATE_DATA]
    assert sum(len(p.payload) - 4 for p in data) == 240
    assert MT.FW_UPDATE_END not in [p.mtype for p in L.sent]


def test_send_image_stop_before_end():
    img = good_image(n=1000)
    L = FakeLink()
    assert fwclient.Client(L, proto.ADDR_BASE).send_image(img, stop_after=len(img)) == "stopped"
    assert MT.FW_UPDATE_END not in [p.mtype for p in L.sent]


def test_send_image_nak_at_start():
    L = FakeLink(nak_type=MT.FW_UPDATE_START)
    res = fwclient.Client(L, proto.ADDR_BASE, retries=0).send_image(good_image())
    assert res.startswith("START NAK")


def test_send_image_crc_fail_status():
    L = FakeLink(end_status=0x03)
    assert fwclient.Client(L, proto.ADDR_BASE).send_image(good_image()) == "END status 0x03"


def test_send_image_on_chunk_callback():
    seen = []
    fwclient.Client(FakeLink(), proto.ADDR_BASE).send_image(good_image(n=1000), on_chunk=lambda off, n: seen.append(off))
    assert seen[0] == 0 and seen[1] == 120


@pytest.mark.parametrize("pct,expect", [(0, 0), (50, 480), (100, 1024)])
def test_offset_at_percent_is_chunk_aligned(pct, expect):
    assert fwclient.offset_at(1024, pct) == expect
