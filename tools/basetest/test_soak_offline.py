"""Offline dry run of the test_soak control flow (without hardware): small fake link / base / remote / log / midi.
Verifies that the soak completes on the happy path and that the sampling-gap and remote-age asserts really fail.
The fake base also models a reliable STATUS_RESP: an unacknowledged reply to GET_STATUS puts, after ~2 s (3 x 500 ms retries),
a W line "retries exhausted" into the log, like the real base (src/router.cpp)."""

import sys
import time

import pytest

import dbg
import proto
import test_soak
from proto import MT, Packet

ADDR_PC, ADDR_BASE, ADDR_REMOTE = proto.ADDR_PC, proto.ADDR_BASE, proto.ADDR_REMOTE
BOOT_ID = 0x1234


class Clock:
    def __init__(self):
        self.t0 = time.monotonic()

    def rel(self):
        return time.monotonic() - self.t0


STATUS_RETRY_S = 2.0  # base: STATUS_RESP 4x at 500 ms, then W "retries exhausted"


class FakeLink:
    """A simulated remote behind the base: ACK for everything PC -> remote, DIAG_GET -> ACK + DIAG_RESP.
    Base: GET_STATUS -> ACK + STATUS_RESP (reliable; without an ACK from the PC within STATUS_RETRY_S -> a W line in the log)."""

    def __init__(self, crc_on_diag=False, reattach_on_diag=False):
        self.q = []
        self.pid = 0
        self.parser = proto.FrameParser()  # PC-side error counters (crc_errors, bad_frames, overflows)
        self.crc_on_diag, self.reattach_on_diag = crc_on_diag, reattach_on_diag
        self.base_pid = 0x60
        self.status_pending = {}  # STATUS_RESP pid -> the time when the base gives up retrying
        self.keepalives_sent = self.keepalive_acks = 0
        self.error = None
        self.stray = b""

    def overdue_status(self):
        now = time.monotonic()
        for pid, deadline in list(self.status_pending.items()):
            if now >= deadline:
                del self.status_pending[pid]
                yield f"[  {now:7.3f}] W router: retries exhausted: type 0x51 to 0x01 pid {pid}"

    def push(self, p):
        self.q.append(p)

    def send(self, p):
        self.pid = (self.pid + 1) & 0xFF
        pid = self.pid
        if p.dst == ADDR_REMOTE and p.mtype != MT.ACK:
            self.push(Packet(ADDR_REMOTE, ADDR_PC, 0, MT.ACK, bytes([pid])))
            if p.mtype == MT.DIAG_GET:
                if self.reattach_on_diag:
                    self.reattach_on_diag = False
                    self.parser = proto.FrameParser()  # Link.attach() replaces the parser
                if self.crc_on_diag:
                    self.parser.crc_errors += 1  # a corrupted base -> PC frame
                self.push(Packet(ADDR_REMOTE, ADDR_PC, 0x40, MT.DIAG_RESP, proto.diag_encode_resp(BOOT_ID, 5, [])))
        elif p.dst == ADDR_BASE and p.mtype == MT.GET_STATUS:
            self.base_pid = (self.base_pid + 1) & 0xFF
            self.push(Packet(ADDR_BASE, ADDR_PC, 0x5F, MT.ACK, bytes([pid])))
            self.push(Packet(ADDR_BASE, ADDR_PC, self.base_pid, MT.STATUS_RESP, bytes([1, 0, 0, 1])))
            self.status_pending[self.base_pid] = time.monotonic() + STATUS_RETRY_S
        elif p.dst == ADDR_BASE and p.mtype == MT.ACK and p.payload:
            self.status_pending.pop(p.payload[0], None)
        return pid

    def send_msg(self, mtype, payload=b""):
        return self.send(Packet(ADDR_PC, ADDR_BASE, 0, mtype, payload))

    def recv(self, timeout, pred=None):
        end = time.monotonic() + timeout
        while True:
            for i, p in enumerate(self.q):
                if pred is None or pred(p):
                    return self.q.pop(i)
            left = end - time.monotonic()
            if left <= 0:
                return None
            time.sleep(min(0.02, left))

    def drain(self):
        q, self.q = self.q, []
        return q


class FakeBase:
    """systick 1 ms; age of the last packet from the remote = age_fn(time since start)."""

    def __init__(self, clock, age_fn):
        self.clock, self.age_fn = clock, age_fn

    def u(self, name, size=4):
        ms = int(self.clock.rel() * 1000) + 10000
        if name == "stmcpp::systick::resolution_":
            return 1
        if name == "stmcpp::systick::ticks_":
            return ms
        if name == "router::_lastRemoteRx":
            return (ms - int(self.age_fn(self.clock.rel()))) & 0xFFFFFFFF
        raise KeyError(name)

    def read_bool(self, name):
        return True

    def fault_record(self):
        return None

    def core_state(self):
        return dict(halted=False, lockup=False)


class FakeRemote:
    """key() takes play_s (PLAY 406+ENTER, sends PLAY_SONG) or back_s (BACK); stale_once: after the first PLAY the digits remain."""

    def __init__(self, link, play_s=0.05, back_s=0.05, stale_once=False):
        self.link, self.play_s, self.back_s, self.stale = link, play_s, back_s, stale_once
        self.digits = 0
        self.calls = []

    def goto_player(self):
        pass

    def u(self, name, size=4):
        if name == "GUI::playerScreen.digitCount_":
            return self.digits
        return 0

    def key(self, *keys):
        self.calls.append(keys)
        if dbg.ENTER in keys:
            time.sleep(self.play_s)
            self.link.push(Packet(ADDR_REMOTE, ADDR_PC, 0x20, MT.PLAY_SONG, b"406"))
            if self.stale:
                self.stale, self.digits = False, 3
        else:
            time.sleep(self.back_s)
            self.digits = 0

    def core_state(self):
        return dict(halted=False, lockup=False)


class FakeLog:
    """history: base log lines; W lines for unacknowledged STATUS_RESPs are added as soon as the base gives up retrying."""

    def __init__(self, link=None):
        self._history = []
        self.link = link

    @property
    def history(self):
        if self.link is not None:
            self._history += [(time.monotonic(), line) for line in self.link.overdue_status()]
        return self._history

    def command(self, cmd):
        return {"level d": ["level D"], "level i": ["level I"]}[cmd]

    def lines(self, timeout, pred=None):
        return ["[   1.000] D router: GET_STATUS"]

    def skip(self):
        pass

    def close(self):
        pass


class FakeWriter:
    def __init__(self, port):
        pass

    def write(self, data):
        pass

    def close(self):
        pass


def happy_age(t):
    return (t * 1000) % 2000 * 0.1  # keepalive replies: the age stays small


@pytest.fixture
def soak_env(monkeypatch, tmp_path):
    monkeypatch.setenv("SOAK_SECONDS", "3")
    monkeypatch.setenv("SOAK_OUT", str(tmp_path))
    monkeypatch.setattr(test_soak, "CYCLE", 1.0)
    monkeypatch.setattr(test_soak, "DIAG_PERIOD", 1.5)
    monkeypatch.setattr(test_soak.midi, "ports", lambda: {"base": "fake"})
    monkeypatch.setattr(test_soak.midi, "Writer", FakeWriter)
    def run(age_fn=happy_age, link_kw=None, **remote_kw):
        clock = Clock()
        link = FakeLink(**(link_kw or {}))
        monkeypatch.setattr(test_soak.loglink, "LogLink", lambda: FakeLog(link))
        remote = FakeRemote(link, **remote_kw)
        test_soak.test_soak(FakeBase(clock, age_fn), link, remote)
        return tmp_path, remote

    return run


def test_soak_dryrun_happy(soak_env, capsys):
    out, remote = soak_env()
    assert (out / "soak.csv").exists() and (out / "summary.txt").exists()
    plays = [k for k in remote.calls if dbg.ENTER in k]
    assert len(plays) >= 2, remote.calls
    assert "SOAK 3 s" in capsys.readouterr().out


def test_fake_base_logs_unacked_status_resp(monkeypatch):
    """The model itself: an unacknowledged STATUS_RESP -> a W line; an acknowledged one -> nothing."""
    monkeypatch.setattr(sys.modules[__name__], "STATUS_RETRY_S", 0.05)
    link = FakeLink()
    log = FakeLog(link)
    link.send_msg(MT.GET_STATUS)
    time.sleep(0.1)
    assert [l for _, l in log.history if " W router: retries exhausted: type 0x51" in l]
    link2 = FakeLink()
    log2 = FakeLog(link2)
    link2.send_msg(MT.GET_STATUS)
    resp = link2.recv(0.1, lambda p: p.mtype == MT.STATUS_RESP)
    link2.send(Packet(ADDR_PC, ADDR_BASE, 0, MT.ACK, bytes([resp.pid])))
    time.sleep(0.1)
    assert log2.history == []


def test_soak_dryrun_slow_play_key_fails_gap(soak_env):
    with pytest.raises(AssertionError, match="age sampling gap"):
        soak_env(play_s=1.2)


def test_soak_dryrun_slow_first_play_key_fails_gap(soak_env, monkeypatch):
    # a single cycle: only the first PLAY in the window is slow (no sample in the cycle before it)
    monkeypatch.setenv("SOAK_SECONDS", "0.5")
    with pytest.raises(AssertionError, match="age sampling gap"):
        soak_env(play_s=1.2)


def test_soak_dryrun_slow_back_key_fails_gap(soak_env):
    with pytest.raises(AssertionError, match="age sampling gap"):
        soak_env(back_s=1.2, stale_once=True)


def test_soak_dryrun_remote_age_over_limit_fails(soak_env):
    with pytest.raises(AssertionError, match="max remote age"):
        soak_env(age_fn=lambda t: 3200.0 if 1.0 <= t < 2.0 else happy_age(t))


def test_soak_dryrun_pc_crc_error_fails(soak_env):
    with pytest.raises(AssertionError, match="pc_crc_errors"):
        soak_env(link_kw=dict(crc_on_diag=True))


def test_soak_dryrun_pc_crc_error_after_reattach_fails(soak_env):
    # Link.attach() swaps the parser: an error in the new parser must be counted too
    with pytest.raises(AssertionError, match="pc_crc_errors"):
        soak_env(link_kw=dict(crc_on_diag=True, reattach_on_diag=True))
