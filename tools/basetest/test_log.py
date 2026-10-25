"""The base text log on the second USB-CDC ("MIDIControl log"), the banner with the reset cause, the log before a reset (no-init RAM)."""

import os
import re
import threading
import time

import pytest

import loglink
import proto
import usblink
from proto import MT, Packet

ADDR_PC, ADDR_BASE = proto.ADDR_PC, proto.ADDR_BASE
BANNER_RE = r"^\[ *\d+\.\d{3}\] I sys: MIDIControl base .* rev [0-9a-f]{7}"
PRE_START = "--- log before reset ---"
PRE_END = "--- end of log before reset ---"
SCB_AIRCR = 0xE000ED0C
AIRCR_SYSRESETREQ = 0x05FA0004  # VECTKEY | SYSRESETREQ, like NVIC_SystemReset()


@pytest.fixture
def log():
    L = loglink.LogLink()
    yield L
    err = L.error
    L.close()
    assert err is None, f"loglink thread failed: {err!r}"


def ack_status(link):
    """GET_STATUS on the protocol CDC: ACK + STATUS_RESP, the reply is acknowledged (otherwise the base would repeat it)."""
    pid = link.send_msg(MT.GET_STATUS)
    ack = link.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.ACK and p.payload[:1] == bytes([pid]))
    resp = link.recv(1.0, lambda p: p.src == ADDR_BASE and p.mtype == MT.STATUS_RESP)
    assert ack is not None and resp is not None
    link.send(Packet(ADDR_PC, ADDR_BASE, 0, MT.ACK, bytes([resp.pid])))


def keepalive_rtt(link) -> float:
    t = time.monotonic()
    link.send_msg(MT.KEEPALIVE)
    r = link.recv(1.0, lambda p: p.mtype == MT.KEEPALIVE_ACK)
    assert r is not None, "KEEPALIVE not answered"
    return time.monotonic() - t


def wait_log_port(timeout=5.0) -> str:
    end = time.monotonic() + timeout
    while True:
        try:
            return loglink.find_log_port()
        except RuntimeError:
            if time.monotonic() > end:
                raise
            time.sleep(0.1)


def reset_and_reopen(link, log, do_reset):
    """Reset the base (`do_reset()`), USB back within 5 s, reopen the log CDC and the protocol link. Returns a new LogLink."""
    log.close()
    link.detach()
    t0 = time.monotonic()
    do_reset()
    time.sleep(1.0)  # the old CDC must disappear (reset -> bootloader -> application)
    port = wait_log_port(5.0)
    assert time.monotonic() - t0 < 6.5, "USB did not come back within 5 s"
    for _ in range(30):
        try:
            new = loglink.LogLink(port)
            break
        except Exception:  # noqa: BLE001 - the port is not openable yet
            time.sleep(0.1)
    else:
        raise AssertionError("log CDC not openable")
    link.attach(settle=0.0)
    return new


def boot_section(log, timeout=5.0) -> list:
    """Lines of the freshly booted base: skips the "log before reset" part (if any), returns the lines up to and including the banner."""
    first = log.lines(timeout, pred=lambda l: l == PRE_START or re.search(BANNER_RE, l))
    if first and first[-1] == PRE_START:
        log.lines(timeout, pred=lambda l: l == PRE_END)
        first = log.lines(timeout, pred=lambda l: re.search(BANNER_RE, l))
    assert first and re.search(BANNER_RE, first[-1]), first
    return first


def test_log_cdc_is_separate(link, log):
    ports = usblink._sysfs_ports()
    assert len(ports) == 2, ports
    by_itf = {p[2]: p for p in ports}
    assert "00" in by_itf, ports
    log_ports = [p for p in ports if p[3] == usblink.LOG_ITF_NAME]
    assert len(log_ports) == 1 and log_ports[0][2] != "00", ports
    assert os.path.basename(os.path.realpath(link.port)) == by_itf["00"][0]
    assert os.path.basename(os.path.realpath(log.port)) == log_ports[0][0]
    keepalive_rtt(link)

    # 2 s of logging (D level, GET_STATUS) - nothing outside frames may appear on the protocol CDC
    assert log.command("level d") == ["level D"]
    try:
        log.skip()
        end = time.monotonic() + 2.0
        while time.monotonic() < end:
            ack_status(link)
            time.sleep(0.05)
    finally:
        log.command("level i")
    assert any(" D router: " in l for l in log.lines(0.5)), "no D lines logged"
    assert link.stray == b""


def test_banner_after_probe_reset(link, log, base):
    log = reset_and_reopen(link, log, base.reset)
    try:
        boot_section(log)
        log.wait_for(r"I sys: reset: .*PIN", 2.0)
    finally:
        log.close()


def test_banner_after_software_reset(link, log, base):
    # = NVIC_SystemReset() (CMSIS inline, not in the symbol table): SCB->AIRCR = VECTKEY | SYSRESETREQ while the core runs
    log = reset_and_reopen(link, log, lambda: base.write32(SCB_AIRCR, AIRCR_SYSRESETREQ))
    try:
        boot_section(log)
        log.wait_for(r"I sys: reset: .*SOFTWARE", 2.0)
    finally:
        log.close()


def test_level_switch(link, log):
    assert log.command("level d") == ["level D"]
    try:
        log.skip()
        ack_status(link)
        log.wait_for(r" D router: ", 2.0)
    finally:
        assert log.command("level i") == ["level I"]
    log.skip()
    for _ in range(5):
        ack_status(link)
    assert not [l for l in log.lines(1.0) if " D " in l]


def test_help_and_unknown(log):
    assert any("level" in l for l in log.command("help"))
    assert log.command("bogus") == ["?"]


def test_pre_reset_log_survives(link, log, base):
    # Guaranteed last I/W lines: toggling DTR of the protocol CDC logs "I usb:" (port close/open)
    link.detach()
    link.attach(settle=0.0)
    time.sleep(0.5)
    iw = [l for l in log.lines(1.0) if re.match(r"^\[ *\d+\.\d{3}\] [IW] ", l)]
    assert iw, "no I/W line before the reset"
    last = iw[-1]
    log = reset_and_reopen(link, log, base.reset)
    try:
        log.wait_for("^" + re.escape(PRE_START) + "$", 5.0)
        before = log.lines(5.0, pred=lambda l: l == PRE_END)
        assert before and before[-1] == PRE_END, before
        assert last in before, (last, before[-10:])
    finally:
        log.close()


def test_ring_wraps_without_host(link, log, base):
    """Log CDC closed, >3 KB of D lines in 10 s: the loop never stalls, after opening "[dropped N]" and whole lines."""
    assert log.command("level d") == ["level D"]
    port = log.port
    log.close()
    worst = 0.0
    stall = []
    stop = threading.Event()

    def watch_core():
        while not stop.is_set():
            st = base.core_state()
            if st["halted"] or st["lockup"]:
                stall.append(st)
            time.sleep(0.2)

    w = threading.Thread(target=watch_core, daemon=True)
    w.start()
    try:
        end = time.monotonic() + 10.0
        n = 0
        while time.monotonic() < end:
            ack_status(link)
            n += 1
            if n % 10 == 0:
                worst = max(worst, keepalive_rtt(link))
            time.sleep(0.02)
    finally:
        stop.set()
        w.join(2)
    assert not stall, stall
    assert worst < 0.2, f"KEEPALIVE took {worst * 1000:.0f} ms"
    assert n * 2 * 40 > 3072, n  # ~2 D lines of >40 B per GET_STATUS

    log2 = loglink.LogLink(port)
    try:
        got = log2.lines(3.0, pred=lambda l: l.startswith("[dropped"))
        assert got and got[-1].startswith("[dropped"), got[-5:]
        m = re.fullmatch(r"\[dropped (\d+)\]", got[-1])
        assert m and int(m.group(1)) > 0
        after = log2.lines(1.5)
        assert after, "no lines after the [dropped] marker"
        assert all(loglink.is_log_line(l) or loglink.is_marker(l) for l in after), after
    finally:
        log2.command("level i")
        log2.close()


def test_line_format(link, log):
    """Every received line (except markers and replies to commands) has the format [ssssss.mmm] L tag: text."""
    assert log.command("level d") == ["level D"]
    try:
        log.skip()
        for _ in range(10):
            ack_status(link)
        link.detach()
        link.attach(settle=0.0)
        time.sleep(0.5)
    finally:
        log.command("level i")
    with log._cv:
        lines = [l for _, l in log.history]
    replies = {"level D", "level I"}
    checked = [l for l in lines if not loglink.is_marker(l) and l not in replies]
    assert checked
    bad = [l for l in checked if not loglink.is_log_line(l)]
    assert not bad, bad


def _crc32(b: bytes) -> int:
    import zlib
    return zlib.crc32(b) & 0xFFFFFFFF


def test_positions_survive_uint32_wrap(link, log, base):
    """head/tail are absolute positions; 2^32 is not a multiple of 1536, so before the wrap they must be shifted by a multiple of
    1536. The positions are moved to just below 2^32 (same ring indices), then logging crosses the boundary: lines on the CDC
    and in RAM must stay whole and the header valid."""
    import struct
    size = 1536
    hdr = base.sym("Debug::_log")
    with base.safe_halt():
        magic, head, tail, dropped, _ = struct.unpack("<5I", base.read(hdr, 20))
        sent = base.u("Debug::_sent")
        boot = base.u("Debug::_bootPos")
        target = 0xFFFFFFFF - 600  # tail just below 2^32: further lines cross the wrap
        delta = ((target - tail) // size) * size
        f = [magic, (head + delta) & 0xFFFFFFFF, (tail + delta) & 0xFFFFFFFF, dropped]
        f.append(_crc32(struct.pack("<4I", *f)))
        base.write(hdr, struct.pack("<5I", *f))
        base.write32(base.sym("Debug::_sent"), (sent + delta) & 0xFFFFFFFF)
        base.write32(base.sym("Debug::_bootPos"), (boot + delta) & 0xFFFFFFFF)
    assert log.command("level d") == ["level D"]
    try:
        log.skip()
        for _ in range(40):  # ~3 KB of lines: head crosses 2^32
            ack_status(link)
        time.sleep(0.5)
    finally:
        log.command("level i")
    got = [l for l in log.lines(1.0) if l != "level I"]
    bad = [l for l in got if not (loglink.is_log_line(l) or loglink.is_marker(l))]
    assert got and not bad, bad
    magic, head, tail, dropped, crc = struct.unpack("<5I", base.read(hdr, 20))
    assert crc == _crc32(struct.pack("<4I", magic, head, tail, dropped))
    assert (head - tail) & 0xFFFFFFFF <= size
    lines = base.debug_log()
    assert lines and all(loglink.is_log_line(l) for l in lines), lines
