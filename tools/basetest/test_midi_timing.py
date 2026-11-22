"""USB-MIDI -> DIN timing of the base, measured ON THE WIRE with the logic analyzer (D5 = base DIN OUT, la.CH["din_out"]).

The PC sends notes with midi.Writer to the base USB-MIDI port; the analyzer (4 MHz, 1 channel, 31250 Bd = 128 samples per bit)
decodes the DIN OUT UART with time. The Otraki USB-MIDI interface (DIN IN of the same wire) is read in parallel for the round trip.
After every scenario the base's main-loop counters (inc/looptime.hpp, read by dbg.Target.loop_stats) are printed.

Scenarios: single notes every 10 ms / 2 ms, 4-note chords every 50 ms (as 4 writes and as one 12-byte write), each idle,
under load (display forced on + CDC KEEPALIVE / NOW_PLAYING_INFO as in test_midi.py) and during BLE reconnect attempts
(remote RN4870 held in reset, as test_midi_latency_during_reconnect).

Limits (proposed by the owner's request, reported even when they fail):
  wire spacing error |dt - period|: p99 <= 1 ms, max <= 2 ms;   chord: first byte -> end of last message <= 4 x 0.96 ms + 0.5 ms.
The worst main-loop pass under load (target <= 1 ms) is only reported.

What cannot be separated: the PC clock (time.monotonic, send and Otraki receive timestamps) and the analyzer clock have an unknown
offset, so the absolute PC -> wire and wire -> Otraki latencies are not separable. Reported are the round trip (an upper bound for
both) and the jitter of each leg (spread after removing a linear clock drift). Set BASETEST_TIMING_JSON=<file> to also dump the numbers.
"""

import json
import os
import threading
import time

import pytest

import dbg
import la
import midi
from conftest import wait_until
from proto import MT

SAMPLERATE = os.environ.get("BASETEST_TIMING_RATE", "4M")
RATE_HZ = float(SAMPLERATE.rstrip("M")) * 1e6
LEAD, TAIL = 2.0, 2.0  # seconds of capture before the first and after the last note (sigrok starts ~0.5 s late)
BYTE_S = 10 / 31250  # 0.32 ms per byte
MSG_S = 3 * BYTE_S  # 0.96 ms per 3-byte message
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


def detrend(x, y):
    """y - (a + b x) by least squares: removes the constant offset and the clock drift between two clocks."""
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    sxx = sum((a - mx) ** 2 for a in x)
    b = sum((a - mx) * (c - my) for a, c in zip(x, y)) / sxx if sxx else 0.0
    return [c - my - b * (a - mx) for a, c in zip(x, y)]


def spread(v):
    return dict(p1=pct(v, 1) * 1e3, p99=pct(v, 99) * 1e3, span=(max(v) - min(v)) * 1e3)


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


def parse(stream, lens=None):
    """[(t, byte)] -> ([(t_first_byte, 3-byte channel message)], [(t_first_byte, n_bytes) of SysEx]). The base sends its display
    state as SysEx F0 4D 43 ... every 500 ms on the DIN too; it is not part of the note stream but occupies the wire.
    Decodes running status (firmware MIDI_RUNNING_STATUS, default since 2026-11-22: a repeated status byte is omitted).
    `lens`, when given, receives the wire byte count of each channel message (3 with its status byte, 2 without)."""
    msgs, sx = [], []
    buf, t0, insx, status, had_status, n = bytearray(), 0.0, False, 0, False, 0
    for t, b in stream:
        if b >= 0xF8:
            continue
        if insx:
            if b < 0x80:
                n += 1
                continue
            insx = False
            if b == 0xF7:
                sx.append((t0, n + 1))
                continue
        if b == 0xF0:
            insx, t0, n, status = True, t, 1, 0
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
            if lens is not None:
                lens.append(3 if had_status else 2)
            buf.clear()
            had_status = False
    return msgs, sx


# ---- one run -----------------------------------------------------------------------------------------------------------------

def sleep_until(t):
    d = t - time.monotonic()
    if d > 0:
        time.sleep(d)


def run(pr, base, kind, period, count):
    """kind 'single' | 'chord4' (4 separate writes) | 'chord4one' (one 12-byte write). Returns a dict of raw data."""
    per = 1 if kind == "single" else 4
    duration = count * period + 0.5
    base.loop_reset()
    proc, out = la.start(LEAD + duration + TAIL, la.chans("din_out"), SAMPLERATE)
    sent, s_ns = [], []  # one entry per message
    with midi.Reader(pr["otraki"]) as rd, midi.Writer(pr["base"]) as w:
        time.sleep(LEAD)
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
        stats = base.loop_stats()  # right after the last note, the capture tail is idle
        time.sleep(0.3)
        ev = list(rd.events)
    la.finish(proc, out, LEAD + duration + TAIL + 60)
    data, warns = la.uart_timed(out, la.CH["din_out"], 31250, RATE_HZ)
    out.unlink(missing_ok=True)
    wire_len = []
    wire, wire_sx = parse([(t, x) for t, x in data], wire_len)
    got, r_t, _ = None, None, None
    rx, rx_sx = parse([(t / 1e9, x) for t, b in ev for x in b])
    rx = [(t, m) for t, m in rx if m[0] == 0x90]  # other traffic (CC from other users of the bench) is not ours
    got = [m for _, m in rx]
    r_ns = [t * 1e9 for t, _ in rx]
    buf = bytearray()
    return dict(sent=sent, s=[x / 1e9 for x in s_ns], wire=wire, wire_len=wire_len, wire_sysex=wire_sx, warns=warns, got=got, r=[x / 1e9 for x in r_ns], stats=stats,
                kind=kind, period=period, count=count)


def analyse(name, d):
    sent, wire, s, r = d["sent"], d["wire"], d["s"], d["r"]
    wmsgs = [m for _, m in wire]
    w = [t for t, _ in wire]
    res = dict(name=name, sent=len(sent), on_wire=len(w), running_status=sum(1 for n in d["wire_len"] if n == 2), sysex_on_wire=len(d["wire_sysex"]),
               sysex_bytes=sorted({n for _, n in d["wire_sysex"]}), decode_warnings=len(d["warns"]), in_order=wmsgs == sent)
    # loss: sent notes missing on the wire
    res["lost"] = len([m for m in sent if m not in set(wmsgs)]) if wmsgs != sent else 0
    if len(w) == len(sent) and wmsgs == sent:
        e = [t + n * BYTE_S for t, n in zip(w, d["wire_len"])]  # end of message on the wire (2 bytes with running status)
        if d["kind"] == "single":
            sp = [(b - a) - d["period"] for a, b in zip(w, w[1:])]
            res["wire_spacing_err_abs_ms"] = summarize([abs(x) for x in sp])
            ssp = [(b - a) - d["period"] for a, b in zip(s, s[1:])]
            res["send_spacing_err_abs_ms"] = summarize([abs(x) for x in ssp])
            res["wire_minus_send_spacing_abs_ms"] = summarize([abs((b - a) - (d - c)) for a, b, c, d in zip(w, w[1:], s, s[1:])])
        else:
            starts, ends, s2s = [], [], []
            for i in range(0, len(w), 4):
                starts.append(w[i + 3] - w[i])
                ends.append(e[i + 3] - w[i])
            res["chord_first_to_last_start_ms"] = summarize(starts)
            res["chord_first_start_to_last_end_ms"] = summarize(ends)
            cp = [(w[i + 4] - w[i]) - d["period"] for i in range(0, len(w) - 4, 4)]
            res["chord_to_chord_err_abs_ms"] = summarize([abs(x) for x in cp])
            res["chord_gap_after_last_end_min_ms"] = min(w[i + 4] - e[i + 3] for i in range(0, len(w) - 4, 4)) * 1e3
        # round trip via the Otraki
        if d["got"] == sent and len(r) == len(s):
            rtt = [b - a for a, b in zip(s, r)]
            res["rtt_ms"] = summarize(rtt)
            leg1 = detrend(s, w)  # PC send -> wire start, offset and drift removed
            leg2 = detrend(e, r)  # wire end -> Otraki receive
            res["jitter_pc_to_wire_ms"] = spread(leg1)
            res["jitter_wire_to_otraki_ms"] = spread(leg2)
        else:
            res["otraki"] = f"got {len(d['got'])} of {len(sent)} (in order: {d['got'] == sent})"
    st = d["stats"]
    res["loop"] = dict(max_pass_us=round(st["max_pass_us"], 1), max_pass_raw_us=round(st["max_pass_raw_us"], 1), passes=st["passes"], hist=st["hist"],
                       sect_max_us={k: round(v, 1) for k, v in st["sect_max_us"].items()}, sect_over_1ms=st["sect_over_1ms"], lat_max_us=round(st["lat_max_us"], 1),
                       lat_first_max_us=round(st["lat_first_max_us"], 1), lat_hist=st["lat_hist"], lat_first_hist=st["lat_first_hist"], msgs=st["msgs"],
                       send_max_us=round(st["send_max_us"], 1))
    f = lambda x: f"{x['p50']:.3f}/{x['p99']:.3f}/{x['max']:.3f}"
    lines = [f"[{name}] sent {len(sent)} wire {len(w)} (running status {res['running_status']}) lost {res['lost']} sysex {res['sysex_on_wire']}x{res['sysex_bytes']} in-order {res['in_order']} uart-warnings {len(d['warns'])}"]
    for k in ("wire_spacing_err_abs_ms", "send_spacing_err_abs_ms", "wire_minus_send_spacing_abs_ms", "chord_first_to_last_start_ms",
              "chord_first_start_to_last_end_ms", "chord_to_chord_err_abs_ms", "rtt_ms"):
        if k in res:
            lines.append(f"  {k} p50/p99/max = {f(res[k])}")
    for k in ("jitter_pc_to_wire_ms", "jitter_wire_to_otraki_ms"):
        if k in res:
            lines.append(f"  {k} p1/p99/span = {res[k]['p1']:.3f}/{res[k]['p99']:.3f}/{res[k]['span']:.3f}")
    if "otraki" in res:
        lines.append(f"  otraki: {res['otraki']}")
    L = res["loop"]
    lines.append(f"  loop: passes {L['passes']} max pass (no DIN send) {L['max_pass_us']} us, raw {L['max_pass_raw_us']} us, hist<50/100/250/500us/1/2/5ms/more {L['hist']}")
    lines.append(f"  loop sections max us {L['sect_max_us']}  over1ms {sum(L['sect_over_1ms'].values())}")
    lines.append(f"  usb->din latency max {L['lat_max_us']} us, first-of-batch max {L['lat_first_max_us']} us, hist {L['lat_hist']}, send max {L['send_max_us']} us")
    res["line"] = "\n".join(lines)
    print("\n" + res["line"])
    RESULTS.append(res)
    return res


def check(res, d):
    assert res["lost"] == 0 and res["in_order"], f"{res['name']}: sent {res['sent']}, on the wire {res['on_wire']}, in order {res['in_order']}"
    assert res["decode_warnings"] == 0, f"{res['name']}: UART decode warnings {d['warns'][:3]}"
    if d["kind"] == "single":
        e = res["wire_spacing_err_abs_ms"]
        assert e["p99"] <= 1.0, f"{res['name']}: wire spacing error p99 {e['p99']:.3f} ms > 1 ms"
        assert e["max"] <= 2.0, f"{res['name']}: wire spacing error max {e['max']:.3f} ms > 2 ms"
    else:
        lim = 4 * MSG_S * 1e3 + 0.5
        c = res["chord_first_start_to_last_end_ms"]
        assert c["max"] <= lim, f"{res['name']}: chord spread max {c['max']:.3f} ms > {lim:.2f} ms"


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
