"""Odolnost parseru base na USB-CDC: rozdelene/slepene ramce, chybne CRC, smeti."""

import random
import time

import pytest

import proto
from proto import MT, NAK, Packet

ADDR_PC, ADDR_BASE, ADDR_REMOTE = proto.ADDR_PC, proto.ADDR_BASE, proto.ADDR_REMOTE

KA = lambda pid: proto.encode(ADDR_PC, ADDR_BASE, pid, MT.KEEPALIVE)  # noqa: E731


@pytest.fixture
def quiet(link):
    link.pause_keepalive(True)
    link.drain()
    try:
        yield link
    finally:
        link.pause_keepalive(False)


def ka_acks(link, timeout=0.6):
    link.recv(timeout, lambda p: False)  # pocka na dobehnuti odpovedi
    return [p for p in link.drain() if p.mtype == MT.KEEPALIVE_ACK and p.src == ADDR_BASE]


def write_unobserved(link, data: bytes):
    """Zapis mimo evidenci keepalive (smeti nesmi rozhodit parovani ACK)."""
    with link._lock:
        link.ser.write(data)
        link.ser.flush()


def test_split_frame_bytewise(quiet):
    frame = KA(0x21)
    for i in range(len(frame)):
        quiet.send_raw(frame[i : i + 1])
        time.sleep(0.005)
    assert len(ka_acks(quiet)) == 1


def test_burst_8_frames(quiet):
    quiet.send_raw(b"".join(KA(0x30 + i) for i in range(8)))
    assert len(ka_acks(quiet, 1.0)) == 8


def test_bad_crc_silent(quiet):
    frame = bytearray(KA(0x22))
    frame[-2] ^= 0x55  # bajt CRC pred EOT
    quiet.send_raw(bytes(frame))
    assert quiet.expect_silence(0.5) == []


def test_truncated_then_good(quiet):
    frame = KA(0x23)
    quiet.send_raw(frame[: len(frame) // 2])
    quiet.send_raw(KA(0x24))
    assert len(ka_acks(quiet)) == 1


def test_300_byte_garbage(quiet):
    rng = random.Random(300)
    write_unobserved(quiet, bytes(rng.choice([b for b in range(256) if b != proto.SOH]) for _ in range(300)))
    quiet.send_raw(KA(0x25))
    assert len(ka_acks(quiet)) == 1


def test_max_escaped_payload(quiet, base, request):
    """Maximalni payload (127 B, sam escapovany) do Remote: base ho cely naparsuje a odpovi NAK UNREACHABLE.
    Je-li remote pripojen, drzi se jeho RN4870 v resetu (PB8 pres sondu remote), aby odpoved byla NAK."""
    from conftest import wait_until
    remote = None
    if base.u("Bluetooth::_connected", 1):
        remote = request.getfixturevalue("remote_probe")  # preskoci, kdyz sondu drzi jiny proces
        remote.hold_ble_reset(True)
    try:
        if remote:
            wait_until(lambda: not base.u("Bluetooth::_connected", 1), 10, what="base sees the remote gone")
            quiet.drain()
        payload = bytes([0x01, 0x04, 0x1B, 0x24, 0x25] * 26)[:127]
        pid = quiet.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.SIGNAL_ANCHOR, payload))
        nak = quiet.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.NAK)
        assert nak is not None and nak.payload == bytes([pid, NAK.UNREACHABLE])
        assert base.core_state()["lockup"] is False
    finally:
        if remote:
            remote.hold_ble_reset(False)
            wait_until(lambda: base.u("Bluetooth::_streamOpen", 1), 15, what="base reconnected to the remote")


def test_random_20x2k(quiet, base):
    rng = random.Random(20)
    for _ in range(20):
        write_unobserved(quiet, rng.randbytes(2048))
    quiet.drain()
    time.sleep(0.3)
    quiet.drain()
    quiet.send_raw(KA(0x26))
    assert len(ka_acks(quiet, 1.0)) == 1
    assert base.core_state()["lockup"] is False
