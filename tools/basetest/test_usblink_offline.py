import threading
import time

import proto
from proto import MT
from usblink import KeepaliveTracker, Link


class FakeSerial:
    """Minimalni serial: read() bere z fronty, write() si zaznamenava a umi simulovat base."""

    def __init__(self):
        self.rx = bytearray()
        self.tx = bytearray()
        self.lock = threading.Lock()
        self.fail_write = False

    def feed(self, b):
        with self.lock:
            self.rx += b

    def read(self, n):
        time.sleep(0.005)
        with self.lock:
            out, self.rx = bytes(self.rx[:n]), self.rx[n:]
        return out

    def write(self, b):
        if self.fail_write:
            raise OSError("boom")
        self.tx += b

    def flush(self):
        pass

    def close(self):
        pass


def ka_ack(pid=9):
    return proto.encode(2, 1, pid, MT.KEEPALIVE_ACK)


def test_tracker_fifo_and_expiry():
    t = KeepaliveTracker(expiry=1.0)
    t.sent(True, now=0)
    t.sent(False, now=0.1)
    assert t.ack(now=0.2) is True    # bg
    assert t.ack(now=0.3) is False   # test
    assert t.ack(now=0.4) is False   # nothing outstanding
    t.sent(True, now=10)
    assert t.ack(now=12) is False    # lost bg ack expired, cannot leak onto later ones


def test_own_keepalive_ack_not_swallowed_while_bg_pending():
    f = FakeSerial()
    L = Link(ser=f)
    L._ka_track.sent(True)  # bg keepalive vyslan, odpoved jeste neprisla
    L.send(proto.Packet(1, 2, 0, MT.KEEPALIVE))
    f.feed(ka_ack(1) + ka_ack(2))  # base odpovida v poradi: bg, pak testovy
    assert L.recv(0.5, lambda p: p.mtype == MT.KEEPALIVE_ACK).pid == 2
    assert L.recv(0.1) is None and L.keepalive_acks == 1
    L.close()


def test_raw_burst_of_keepalives_is_tracked():
    f = FakeSerial()
    L = Link(ser=f)
    L.send_raw(b"".join(proto.encode(1, 2, i, MT.KEEPALIVE) for i in range(8)), chunk=3)
    f.feed(b"".join(ka_ack(i) for i in range(8)))
    got = [L.recv(0.5, lambda p: p.mtype == MT.KEEPALIVE_ACK) for _ in range(8)]
    assert all(got) and L.keepalive_acks == 0
    L.close()


def test_base_keepalive_answered_and_delivered_and_pausable():
    f = FakeSerial()
    L = Link(ser=f)
    f.feed(proto.encode(2, 1, 5, MT.KEEPALIVE))
    assert L.recv(0.5, lambda p: p.mtype == MT.KEEPALIVE) is not None
    time.sleep(0.05)
    assert len(f.tx) > 0
    f.tx.clear()
    L.pause_keepalive()
    f.feed(proto.encode(2, 1, 6, MT.KEEPALIVE))
    assert L.recv(0.5, lambda p: p.mtype == MT.KEEPALIVE) is not None
    time.sleep(0.05)
    assert bytes(f.tx) == b""
    L.close()


def test_thread_error_is_raised():
    f = FakeSerial()
    L = Link(ser=f)
    f.fail_write = True
    f.feed(proto.encode(2, 1, 5, MT.KEEPALIVE))  # auto-ACK selze v reader vlakne
    time.sleep(0.2)
    assert isinstance(L.error, OSError)
    try:
        L.recv(0.1)
        assert False
    except RuntimeError:
        pass
    L.error = None
    L.close()


def test_background_keepalive_runs_and_pauses():
    f = FakeSerial()
    L = Link(ser=f, keepalive=True, keepalive_period=0.05)
    time.sleep(0.3)
    assert L.keepalives_sent >= 3
    L.pause_keepalive()
    n = L.keepalives_sent
    time.sleep(0.2)
    assert L.keepalives_sent == n
    L.close()
