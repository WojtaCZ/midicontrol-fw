"""Relay PC <-> Remote via the base (USB-CDC <-> BLE) with a paired remote. The PC is played by the harness (link), the remote is real
(keys and state via its ST-Link, `remote_probe`). Precondition: the base and the remote are paired (test_pairing.py)."""

import re
import time

import pytest

import dbg
import la
import proto
from conftest import remote_alive, wait_until
from proto import MT, NAK, Packet

ADDR_PC, ADDR_BASE, ADDR_REMOTE = proto.ADDR_PC, proto.ADDR_BASE, proto.ADDR_REMOTE
BAUD = 115200
MAX_RETRIES = 3  # PROTOCOL.md: 3 retries at 500 ms, same PacketId


def from_remote(mtype, pid=None):
    return lambda p: p.src == ADDR_REMOTE and p.dst == ADDR_PC and p.mtype == mtype and (pid is None or p.pid == pid)


def reply_to(pid, src=ADDR_REMOTE):
    return lambda p: p.src == src and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid])


def to_remote(link, mtype, payload=b"") -> int:
    return link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, mtype, payload))


def pc_signal(link, mtype, payload=b""):
    """PC -> Remote, waits for an ACK from the remote (src 3)."""
    pid = to_remote(link, mtype, payload)
    r = link.recv(1.5, reply_to(pid))
    assert r is not None and r.mtype == MT.ACK, f"{MT(mtype).name}: {r}"
    return r


def pc_ack(link, pkt):
    link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.ACK, bytes([pkt.pid])))


def frames(data: bytes) -> list:
    return proto.FrameParser().feed(data)


@pytest.fixture(scope="module")
def paired(base):
    """The link to the remote must be up (stream open, secured) and the remote alive."""
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="BLE stream open (pair first: test_pairing.py)")
    wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
    return base


@pytest.fixture
def player(paired, link, remote_probe):
    """Remote on the player screen, IDLE, empty input; no pending commands."""
    remote = remote_probe
    time.sleep(2.2)  # a possible in-progress command from the previous test finishes
    remote.goto_player()
    pc_signal(link, MT.SIGNAL_STOPPED)
    n = remote.u("GUI::playerScreen.digitCount_", 1) + (1 if remote.u("GUI::playerScreen.letter_", 1) else 0)
    if n:
        remote.key(*([dbg.BACK] * n))
    assert remote.player_state() == dbg.IDLE
    link.drain()
    return link, remote


def test_status_ble_connected(paired, link):
    link.drain()
    link.send_msg(MT.GET_STATUS)
    r = link.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.STATUS_RESP)
    assert r is not None
    assert r.payload[0] == 1, f"bleConnected {r.payload.hex()}"


def test_base_keepalive_to_remote(paired):
    base = paired
    sr = la.capture(11, channels="D2,D3", samplerate="1M")
    data, _ = la.uart_timed(sr, la.CH["base_ble_tx"], BAUD, 1e6)
    # frames with the time of the first byte (SOH)
    times, parser = [], proto.FrameParser()
    for t, b in data:
        for p in parser.feed(bytes([b])):
            if p.src == ADDR_BASE and p.dst == ADDR_REMOTE and p.mtype == MT.KEEPALIVE:
                times.append(t)
    gaps = [round(b - a, 3) for a, b in zip(times, times[1:])]
    assert len(times) >= 5, f"{len(times)} keepalives in 11 s, gaps {gaps}"
    assert all(1.8 <= g <= 2.2 for g in gaps), gaps
    assert remote_alive(base)


def test_pc_signal_reaches_remote(player):
    link, remote = player
    pid = to_remote(link, MT.SIGNAL_PLAYING, b"406A")
    r = link.recv(1.5, reply_to(pid))
    assert r is not None and r.mtype == MT.ACK and r.src == ADDR_REMOTE and r.payload == bytes([pid]), r
    wait_until(lambda: remote.player_state() == dbg.PLAYING, 2, what="remote PlayerScreen PLAYING")


def test_remote_play_reaches_pc(player):
    link, remote = player
    remote.key("4", "0", "6", dbg.ENTER)
    pkt = link.recv(2.0, from_remote(MT.PLAY_SONG))
    assert pkt is not None and pkt.payload == b"406", pkt
    pc_ack(link, pkt)
    again = [p for p in link.expect_silence(1.5) if p.mtype == MT.PLAY_SONG and p.pid == pkt.pid]
    assert again == [], f"retransmitted after ACK: {again}"
    pc_signal(link, MT.SIGNAL_STOPPED)


def test_remote_retry_without_pc_ack(player):
    link, remote = player
    remote.key("4", "0", "6", dbg.ENTER)
    first = link.recv(2.0, from_remote(MT.PLAY_SONG))
    assert first is not None
    rest = []
    end = time.monotonic() + 2.5
    while time.monotonic() < end:
        p = link.recv(end - time.monotonic(), from_remote(MT.PLAY_SONG))
        if p is None:
            break
        rest.append(p)
    pkts = [first] + rest
    assert len(pkts) == 1 + MAX_RETRIES, f"{len(pkts)} transmissions"
    assert {p.pid for p in pkts} == {first.pid}, "retransmissions reuse the PacketId (base forwards, no dedupe)"
    times = [t for t, d, p in link.log if d == "rx" and p.mtype == MT.PLAY_SONG and p.pid == first.pid]
    gaps = [round(b - a, 2) for a, b in zip(times, times[1:])]
    assert all(0.4 <= g <= 0.75 for g in gaps), gaps
    pc_signal(link, MT.SIGNAL_STOPPED)


def test_signal_anchor_and_replay(player):
    link, remote = player
    pc_signal(link, MT.SIGNAL_PLAYING, b"401")
    pc_signal(link, MT.SIGNAL_ANCHOR, b"Sloka 2\0")
    assert remote.cstr("GUI::playerScreen.anchorName_", 24) == "Sloka 2"
    remote.key(dbg.RIGHT)
    p = link.recv(2.0, from_remote(MT.REPLAY_ANCHOR))
    assert p is not None and p.src == ADDR_REMOTE, p
    pc_ack(link, p)
    pc_signal(link, MT.SIGNAL_STOPPED)


def test_song_list_to_remote(player):
    link, remote = player
    remote.key(dbg.UP)
    req = link.recv(2.0, from_remote(MT.GET_SONG_LIST))
    assert req is not None and req.payload == b"\x00", req
    pc_ack(link, req)
    pc_signal(link, MT.SONG_LIST_RESP, b"\x01" + b"401;403;406A;406B")
    assert remote.screen_name() == "songListScreen"
    remote.key(dbg.BACK)


def test_pc_to_remote_unreachable(paired, link, remote_probe):
    base, remote = paired, remote_probe
    link.drain()
    proc, sr = la.start(30, channels="D2,D3,D6,D7", samplerate="1M")
    time.sleep(1.0)
    nak_pid = None
    try:
        try:
            remote.hold_ble_reset(True)
            wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 10, what="base sees %DISCONNECT%")
            # GET_STATUS: bleConnected = 0
            link.send_msg(MT.GET_STATUS)
            st = link.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.STATUS_RESP)
            assert st is not None and st.payload[0] == 0, st
            # PC -> Remote without a link: NAK UNREACHABLE from the base immediately
            nak_pid = to_remote(link, MT.SIGNAL_STOPPED)
            t = time.monotonic()
            nak = link.recv(1.0, reply_to(nak_pid, src=ADDR_BASE))
            took = time.monotonic() - t
            assert nak is not None and nak.mtype == MT.NAK and nak.payload == bytes([nak_pid, NAK.UNREACHABLE]), nak
            assert took < 0.2, f"NAK after {took * 1000:.0f} ms"
        finally:
            remote.hold_ble_reset(False)
        t_rel = time.monotonic()
        wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 10, what="base reconnected within 10 s")
        wait_until(lambda: base.read_bool("Bluetooth::_bonded"), 3, what="reconnected link secured")
        reconnect_s = time.monotonic() - t_rel
        time.sleep(0.5)
        pc_signal(link, MT.SIGNAL_STOPPED)
        link.drain()
        link.send_msg(MT.GET_STATUS)
        st = link.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.STATUS_RESP)
        assert st is not None and st.payload[0] == 1, st
    finally:
        la.finish(proc, sr, 60)
        status = {k: re.findall(r"%([^%\r\n]{1,60})%", la.uart(sr, la.CH[k], BAUD).decode("latin1")) for k in ("base_ble_rx", "rem_ble_rx")}
        print("status lines:", status)
    # nothing stale: a packet rejected with a NAK must not reach the remote even after the link is restored
    to_rem = [p for p in frames(la.uart(sr, la.CH["rem_ble_rx"], BAUD)) if p.src == ADDR_PC]
    assert not [p for p in to_rem if p.pid == nak_pid and p.mtype == MT.SIGNAL_STOPPED], to_rem
    print(f"reconnect {reconnect_s:.1f} s after release; PC frames at remote: {[(p.pid, MT(p.mtype).name) for p in to_rem]}")
