"""USB-MIDI <-> DIN via the Otraki USB-MIDI interface (fc02:0101) wired to the base DIN IN/OUT.
PC -> base USB-MIDI -> DIN OUT -> Otraki -> PC and back. The latency is measured by timed rawmidi reads (midi.Reader, ~1 ms)."""

import threading
import time

import pytest

import midi
from conftest import remote_alive, wait_until
from proto import MT

ECHO_PREFIX = bytes([0xF0, 0x4D, 0x43])  # display echo from the base (every 500 ms), not the subject of the test
MAX_GAP = 0.030


@pytest.fixture(scope="module")
def pr():
    return midi.ports()


def msgs(data: bytes) -> list:
    return [m for m in midi.decode(data) if not m.startswith(ECHO_PREFIX)]


def exchange(src, dst, hexstr, wait=0.5):
    """Send to `src`, return the messages received on `dst` (the reader starts before sending)."""
    with midi.Reader(dst) as r:
        time.sleep(0.1)
        midi.send(src, hexstr)
        time.sleep(wait)
        return r.data()


NOTES = [bytes([0x90, 0x3C, 0x64]), bytes([0x80, 0x3C, 0x00])]


def test_usb_to_din_notes(pr):
    got = exchange(pr["base"], pr["otraki"], "90 3C 64 80 3C 00")
    # The base DIN output sends full status bytes; running status (90 3C 64 3C 00) would also be valid
    assert msgs(got) == [bytes([0x90, 0x3C, 0x64]), bytes([0x80, 0x3C, 0x00])], got.hex()


def test_din_to_usb_notes(pr):
    got = exchange(pr["otraki"], pr["base"], "90 3C 64 80 3C 00")
    assert msgs(got) == [bytes([0x90, 0x3C, 0x64]), bytes([0x80, 0x3C, 0x00])], got.hex()


def test_din_to_usb_running_status(pr):
    got = exchange(pr["otraki"], pr["base"], "90 3C 64 3E 64 40 64")
    assert msgs(got) == [bytes([0x90, n, 0x64]) for n in (0x3C, 0x3E, 0x40)], got.hex()


def test_sysex_both_ways(pr):
    sx = bytes([0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7])
    assert msgs(exchange(pr["base"], pr["otraki"], sx.hex())) == [sx]
    assert msgs(exchange(pr["otraki"], pr["base"], sx.hex())) == [sx]


def test_realtime_inside_sysex(pr):
    got = exchange(pr["otraki"], pr["base"], "F0 7D 01 F8 02 F7")
    m = msgs(got)
    assert bytes([0xF8]) in m, got.hex()
    assert bytes([0xF0, 0x7D, 0x01, 0x02, 0xF7]) in m, got.hex()
    assert len(m) == 2, got.hex()


def stream(pr, count=600, period=0.010, during=None):
    """Sends `count` NOTE ONs (each with a different velocity/note) every `period` to the base USB -> DIN; returns (sent, received times).
    Read on the Otraki; gap = difference of the arrival times of adjacent messages (resolution ~1 ms, see midi.py)."""
    sent = []
    with midi.Reader(pr["otraki"]) as r, midi.Writer(pr["base"]) as w:
        time.sleep(0.2)
        t0 = time.monotonic()
        for i in range(count):
            target = t0 + i * period
            d = target - time.monotonic()
            if d > 0:
                time.sleep(d)
            msg = bytes([0x90, 0x30 + i % 24, 1 + i % 127])
            sent.append(msg)
            w.write(msg)
        time.sleep(0.5)
        ev = list(r.events)
    # messages with the time of the first byte (note-ons are 3 bytes each, different read()s may split a message)
    buf, times, got = bytearray(), [], []
    for t, b in ev:
        for x in b:
            buf.append(x)
            if len(buf) == 3:
                got.append(bytes(buf))
                times.append(t / 1e9)
                buf.clear()
    return sent, got, times


def check_stream(sent, got, times):
    got_f = [g for g in got if g[0] == 0x90]
    assert got_f == sent, f"lost/changed: sent {len(sent)}, got {len(got_f)}"
    gaps = [b - a for a, b in zip(times, times[1:])]
    worst = max(gaps)
    print(f"\nmax gap {worst * 1000:.1f} ms, mean {sum(gaps) / len(gaps) * 1000:.2f} ms over {len(gaps)} gaps")
    assert worst < MAX_GAP, f"max inter-arrival gap {worst * 1000:.1f} ms >= {MAX_GAP * 1000:.0f} ms"
    return worst


def test_midi_latency_during_reconnect(pr, base, remote_probe, link):
    """The remote holds its RN4870 in reset -> the base repeatedly tries to connect (C every 5 s). The stream must not stutter."""
    remote = remote_probe
    remote.hold_ble_reset(True)
    try:
        wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 15, what="base lost the remote")
        link.drain()
        sent, got, times = stream(pr)
        assert not base.read_bool("Bluetooth::_connected")
        check_stream(sent, got, times)
    finally:
        remote.hold_ble_reset(False)
        try:  # after the release the remote must re-secure, so that other tests are not disturbed
            wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 40, what="BLE stream open again")
            wait_until(lambda: remote_alive(base), 20, interval=0.5, what="remote alive again")
        except AssertionError as e:
            print(f"WARNING: remote did not re-secure: {e}")


def test_midi_with_display_and_cdc_traffic(pr, base, link):
    """The same stream plus NOW_PLAYING_INFO every 100 ms and KEEPALIVE every 50 ms on the CDC (display forced as in display_on)."""
    link.pause_keepalive(True)
    link.drain()
    addr = base.sym("display::connected")
    orig = base.read(addr, 1)
    stop = threading.Event()
    errs = []

    def cdc():
        n = 0
        t0 = time.monotonic()
        try:
            while not stop.is_set():
                n += 1
                if n % 2 == 1:
                    link.send_msg(MT.KEEPALIVE)
                if n % 4 == 0:  # every 4th tick of 25 ms = 100 ms
                    link.send_msg(MT.NOW_PLAYING_INFO, bytes([0xF4, 0x06, 0xF1, 0x0A, 0x02 if (n // 4) % 2 else 0x01]))
                time.sleep(max(0, t0 + n * 0.025 - time.monotonic()))
        except Exception as e:  # noqa
            errs.append(e)

    base.write(addr, b"\x01")
    th = threading.Thread(target=cdc, daemon=True)
    th.start()
    try:
        sent, got, times = stream(pr)
        check_stream(sent, got, times)
    finally:
        stop.set()
        th.join(2)
        try:
            link.send_msg(MT.NOW_PLAYING_INFO, b"")
            time.sleep(0.2)
        finally:
            base.write(addr, orig)
            link.pause_keepalive(False)
    assert errs == []
