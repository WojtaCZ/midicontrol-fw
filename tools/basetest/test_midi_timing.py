"""USB-MIDI -> DIN path of the base under load: nothing lost, order kept, and the base's own main-loop counters.

No wire timing (the logic analyzer was dropped, owner 2026-11-22): the PC sends notes with midi.Writer to the base USB-MIDI port,
the Otraki USB-MIDI interface (DIN IN of the same wire) is read in parallel and must deliver exactly the notes sent, in order.
The base's main-loop counters (inc/looptime.hpp, read by dbg.Target.loop_stats) give what the STM saw: DIN messages sent (msgs),
USB -> DIN latency (lat_max_us, lat_first_max_us), the worst main-loop pass and the slowest DIN send. They are reported, not limited.

Scenarios: single notes every 10 ms / 2 ms, 4-note chords every 50 ms (as 4 writes and as one 12-byte write), each idle,
under load (display forced on + CDC KEEPALIVE / NOW_PLAYING_INFO as in test_midi.py) and during BLE reconnect attempts
(remote RN4870 held in reset, as test_midi_latency_during_reconnect).

Set BASETEST_TIMING_JSON=<file> to also dump the numbers.
"""

import json
import os
import threading
import time

import pytest

import dbg
import midi
from conftest import wait_until
from proto import MT

RESULTS = []


@pytest.fixture(scope="module")
def pr():
    return midi.ports()


@pytest.fixture(scope="module", autouse=True)
def summary():
    yield
    print("\n\n===== SUMMARY =====")
    for r in RESULTS:
        print(r["line"])
    path = os.environ.get("BASETEST_TIMING_JSON")
    if path:
        with open(path, "w") as f:
            json.dump(RESULTS, f, indent=1)


# ---- statistics --------------------------------------------------------------------------------------------------------------

def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(p / 100 * (len(s) - 1)))))]


def summarize(v):
    """(p50, p99, max, min) in ms of absolute values / raw values given in seconds."""
    return dict(p50=pct(v, 50) * 1e3, p99=pct(v, 99) * 1e3, max=max(v) * 1e3, min=min(v) * 1e3)


# ---- load generators -----------------------------------------------------------------------------------------------------------

class CdcLoad:
    """Display forced on + KEEPALIVE every 50 ms and NOW_PLAYING_INFO every 100 ms on the CDC (as test_midi.py)."""

    def __init__(self, link, base):
        self.link, self.base = link, base
        self.addr = base.sym("display::connected")
        self.errs = []

    def __enter__(self):
        self.link.pause_keepalive(True)
        self.link.drain()
        self.orig = self.base.read(self.addr, 1)
        self.stop = threading.Event()

        def run():
            n = 0
            t0 = time.monotonic()
            try:
                while not self.stop.is_set():
                    n += 1
                    if n % 2 == 1:
                        self.link.send_msg(MT.KEEPALIVE)
                    if n % 4 == 0:
                        self.link.send_msg(MT.NOW_PLAYING_INFO, bytes([0xF4, 0x06, 0xF1, 0x0A, 0x02 if (n // 4) % 2 else 0x01]))
                    time.sleep(max(0, t0 + n * 0.025 - time.monotonic()))
            except Exception as e:  # noqa
                self.errs.append(e)

        self.base.write(self.addr, b"\x01")
        self.th = threading.Thread(target=run, daemon=True)
        self.th.start()
        return self

    def __exit__(self, *a):
        self.stop.set()
        self.th.join(2)
        try:
            self.link.send_msg(MT.NOW_PLAYING_INFO, b"")
            time.sleep(0.2)
        finally:
            self.base.write(self.addr, self.orig)
            self.link.pause_keepalive(False)
        assert self.errs == []


class BleRetry:
    """The remote holds its RN4870 in reset -> the base keeps trying to connect (C every 5 s)."""

    def __init__(self, base, remote):
        self.base, self.remote = base, remote

    def __enter__(self):
        self.remote.hold_ble_reset(True)
        wait_until(lambda: not self.base.read_bool("Bluetooth::_connected"), 15, what="base lost the remote")
        return self

    def __exit__(self, *a):
        self.remote.hold_ble_reset(False)
        try:
            wait_until(lambda: self.base.read_bool("Bluetooth::_streamOpen"), 40, what="BLE stream open again")
        except AssertionError as e:
            print(f"WARNING: remote did not re-secure: {e}")


def parse(stream):
    """[(t, byte)] -> [(t_first_byte, 3-byte channel message)]. Skips SysEx (the base sends its display state as SysEx F0 4D 43 ...
    every 500 ms on the DIN too) and decodes running status (firmware MIDI_RUNNING_STATUS: a repeated status byte is omitted)."""
    msgs = []
    buf, t0, insx, status, had_status = bytearray(), 0.0, False, 0, False
    for t, b in stream:
        if b >= 0xF8:
            continue
        if insx:
            if b < 0x80:
                continue
            insx = False
            if b == 0xF7:
                continue
        if b == 0xF0:
            insx, status = True, 0
            buf.clear()
            continue
        if b >= 0x80:
            status = b if b < 0xF0 else 0
            buf.clear()
            t0, had_status = t, True
            continue
        if not status:
            continue
        if not buf and not had_status:
            t0 = t
        buf.append(b)
        if len(buf) == 2:
            msgs.append((t0, bytes([status]) + bytes(buf)))
            buf.clear()
            had_status = False
    return msgs


# ---- one run -----------------------------------------------------------------------------------------------------------------

def sleep_until(t):
    d = t - time.monotonic()
    if d > 0:
        time.sleep(d)


def run(pr, base, kind, period, count):
    """kind 'single' | 'chord4' (4 separate writes) | 'chord4one' (one 12-byte write). Returns a dict of raw data."""
    per = 1 if kind == "single" else 4
    base.loop_reset()
    sent, s_ns = [], []  # one entry per message
    with midi.Reader(pr["otraki"]) as rd, midi.Writer(pr["base"]) as w:
        time.sleep(0.5)
        t0 = time.monotonic() + 0.05
        for i in range(count):
            sleep_until(t0 + i * period - 0.0004)  # last 0.4 ms: sleep granularity, the actual write time is recorded anyway
            msgs = [bytes([0x90, 0x30 + (i * per + k) % 24, 1 + (i * per + k) % 127]) for k in range(per)]
            if kind == "chord4one":
                s_ns.extend([time.monotonic_ns()] * per)
                w.write(b"".join(msgs))
            else:
                for m in msgs:
                    s_ns.append(time.monotonic_ns())
                    w.write(m)
            sent.extend(msgs)
        time.sleep(0.3)
        stats = base.loop_stats()  # right after the last note
        time.sleep(0.3)
        ev = list(rd.events)
    rx = parse([(t / 1e9, x) for t, b in ev for x in b])
    rx = [(t, m) for t, m in rx if m[0] == 0x90]  # other traffic (CC from other users of the bench) is not ours
    return dict(sent=sent, s=[x / 1e9 for x in s_ns], got=[m for _, m in rx], r=[t for t, _ in rx], stats=stats,
                kind=kind, period=period, count=count)


def analyse(name, d):
    sent, got, s, r, st = d["sent"], d["got"], d["s"], d["r"], d["stats"]
    res = dict(name=name, sent=len(sent), received=len(got), in_order=got == sent, din_msgs=st["msgs"],
               lost=len([m for m in sent if m not in set(got)]) if got != sent else 0)
    if got == sent and len(r) == len(s):
        res["rtt_ms"] = summarize([b - a for a, b in zip(s, r)])  # PC send -> Otraki receive (PC clock only)
    res["loop"] = dict(max_pass_us=round(st["max_pass_us"], 1), max_pass_raw_us=round(st["max_pass_raw_us"], 1), passes=st["passes"], hist=st["hist"],
                       sect_max_us={k: round(v, 1) for k, v in st["sect_max_us"].items()}, sect_over_1ms=st["sect_over_1ms"], lat_max_us=round(st["lat_max_us"], 1),
                       lat_first_max_us=round(st["lat_first_max_us"], 1), lat_hist=st["lat_hist"], lat_first_hist=st["lat_first_hist"], msgs=st["msgs"],
                       send_max_us=round(st["send_max_us"], 1))
    f = lambda x: f"{x['p50']:.3f}/{x['p99']:.3f}/{x['max']:.3f}"
    lines = [f"[{name}] sent {len(sent)} at Otraki {len(got)} lost {res['lost']} in-order {res['in_order']} din msgs (STM) {st['msgs']}"]
    if "rtt_ms" in res:
        lines.append(f"  rtt_ms p50/p99/max = {f(res['rtt_ms'])}")
    L = res["loop"]
    lines.append(f"  loop: passes {L['passes']} max pass (no DIN send) {L['max_pass_us']} us, raw {L['max_pass_raw_us']} us, hist<50/100/250/500us/1/2/5ms/more {L['hist']}")
    lines.append(f"  loop sections max us {L['sect_max_us']}  over1ms {sum(L['sect_over_1ms'].values())}")
    lines.append(f"  usb->din latency max {L['lat_max_us']} us, first-of-batch max {L['lat_first_max_us']} us, hist {L['lat_hist']}, send max {L['send_max_us']} us")
    res["line"] = "\n".join(lines)
    print("\n" + res["line"])
    RESULTS.append(res)
    return res


def check(res, d):
    assert res["lost"] == 0 and res["in_order"], f"{res['name']}: sent {res['sent']}, at the Otraki {res['received']}, in order {res['in_order']}"
    assert res["din_msgs"] >= res["sent"], f"{res['name']}: the STM sent only {res['din_msgs']} DIN messages for {res['sent']} notes"


SCEN = [("single10", "single", 0.010, 300), ("single2", "single", 0.002, 1000), ("chord4", "chord4", 0.050, 100), ("chord4one", "chord4one", 0.050, 100)]


@pytest.mark.parametrize("name,kind,period,count", SCEN, ids=[s[0] for s in SCEN])
def test_idle(pr, base, link, name, kind, period, count):
    """a), b), d), e): PC alive (keepalive on the CDC), nothing else."""
    d = run(pr, base, kind, period, count)
    check(analyse(f"idle/{name}", d), d)


@pytest.mark.parametrize("name,kind,period,count", SCEN, ids=[s[0] for s in SCEN])
def test_load_display_cdc(pr, base, link, name, kind, period, count):
    """c): display forced on + CDC traffic."""
    with CdcLoad(link, base):
        d = run(pr, base, kind, period, count)
    check(analyse(f"load/{name}", d), d)


@pytest.mark.parametrize("name,kind,period,count", SCEN, ids=[s[0] for s in SCEN])
def test_load_ble_retry(pr, base, link, remote_probe, name, kind, period, count):
    """c): display + CDC load while the base repeatedly tries to reach the remote (BLE command mode traffic)."""
    with BleRetry(base, remote_probe), CdcLoad(link, base):
        d = run(pr, base, kind, period, count)
    check(analyse(f"load+ble/{name}", d), d)
