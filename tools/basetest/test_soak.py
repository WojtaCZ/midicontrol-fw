"""Soak celeho systemu (Task 7C): remote (klavesy pres ST-Link) <-> base <-> PC harness, `SOAK_SECONDS` (default 600).
Cyklus (~3 s): PLAY z remote (klavesy 406 + ENTER) -> harness potvrdi, SIGNAL_PLAYING, 3x SIGNAL_ANCHOR, NOW_PLAYING_INFO,
USB-MIDI noty na base (DIN nelze overit, bench DIN link je rozbity: jen se pozoruje, ze zapis projde a log je cisty),
SIGNAL_STOPPED. Kazdych 10 s DIAG_GET na remote s merenim latence DIAG_GET -> DIAG_RESP (R28). Pocitadla po iteracich
jdou do CSV (`SOAK_OUT`, default <tmp>/basetest-soak/soak.csv), na konci se vypise souhrn.
Pass = vsechna pocitadla chyb nulova, boot_id remote stejny, base log bez E/W (krome znamych), uptime base monotonni,
zadne jadro nezastavene/lockup. Latence se jen hlasi (hranice 2 s se pocita, ale neni podminkou)."""

import csv
import os
import re
import statistics
import tempfile
import time

import pytest

import dbg
import loglink
import midi
import proto
from conftest import remote_alive, wait_until
from proto import MT, Packet
from test_relay import from_remote, pc_ack, reply_to
from test_remote_diag import CODE_FAULT, CODE_LOOP_STALL, decode

ADDR_PC, ADDR_BASE, ADDR_REMOTE = proto.ADDR_PC, proto.ADDR_BASE, proto.ADDR_REMOTE
DIAG_PERIOD = 10.0
CYCLE = 3.0
SLOW_S = 2.0
# radky logu base, ktere v soaku nevadi (regex na cely radek); doplnit podle prvniho behu
KNOWN_LOG = [r"^\[ *\d+\.\d{3}\] [EW] [a-z]{1,6}: (?!XXXXXX)NEVER$"]
LOG_TS = re.compile(r"^\[ *(\d+)\.(\d{3})\] ([EWID]) ([a-z]{1,6}): (.*)$")


@pytest.fixture(scope="module")
def paired(base):
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="BLE stream open (pair first: test_pairing.py)")
    wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
    return base


def diag_timed(link, cursor):
    """DIAG_GET -> (ack_s, resp_s, resp); None kdyz chybi. Cas DIAG_RESP od odeslani DIAG_GET (do 8 s, bez opakovani)."""
    link.drain()
    t0 = time.monotonic()
    pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.DIAG_GET, proto.diag_encode_get(cursor)))
    ack = link.recv(8.0, reply_to(pid))
    t_ack = time.monotonic() - t0
    if ack is None or ack.mtype != MT.ACK:
        return None, None, ack
    resp = link.recv(8.0 - t_ack, lambda p: p.src == ADDR_REMOTE and p.dst == ADDR_PC and p.mtype == MT.DIAG_RESP)
    if resp is None:
        return t_ack, None, None
    t_resp = time.monotonic() - t0
    link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.ACK, bytes([resp.pid])))
    return t_ack, t_resp, resp


def test_soak(paired, link, remote_probe):
    base, remote = paired, remote_probe
    duration = float(os.environ.get("SOAK_SECONDS", "600"))
    out_dir = os.environ.get("SOAK_OUT", os.path.join(tempfile.gettempdir(), "basetest-soak"))
    os.makedirs(out_dir, exist_ok=True)
    pr = midi.ports()
    log = loglink.LogLink()
    c = dict(cycles=0, play_missed=0, play_retrans=0, ack_missed=0, nak=0, crc_err=0, diag_polls=0, diag_no_ack=0,
             diag_no_resp=0, diag_nak=0, diag_slow=0, midi_err=0, halted=0, lockup=0, remote_dead=0, digits_stale=0,
             fault_recs=0, boot_changed=0, ble_disconnected=0, max_remote_age_ms=0.0)
    lat_ack, lat_resp, play_lat = [], [], []
    errors = []

    def ack_for(mtype, payload=b""):
        pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, mtype, payload))
        t0 = time.monotonic()
        r = link.recv(1.5, reply_to(pid))
        if r is None:
            c["ack_missed"] += 1
            errors.append(f"{time.monotonic() - t_start:.0f}s: no ACK for {MT(mtype).name}")
        elif r.mtype != MT.ACK:
            c["nak"] += 1
            errors.append(f"{time.monotonic() - t_start:.0f}s: NAK for {MT(mtype).name}: {r}")
        return time.monotonic() - t0

    remote.goto_player()
    ack_for(MT.SIGNAL_STOPPED)
    n = remote.u("GUI::playerScreen.digitCount_", 1) + (1 if remote.u("GUI::playerScreen.letter_", 1) else 0)
    if n:
        remote.key(*([dbg.BACK] * n))
    link.drain()
    log.skip()
    fault0 = base.fault_record()  # zbytek z drivejsich testu (.noinit): nesmi se zmenit
    # pocatecni stav
    t_ack, t_resp, resp = diag_timed(link, 0xFFFF)
    assert resp is not None, "no DIAG_RESP at start"
    boot_id, cur, _ = decode(resp)
    ka_sent0, ka_ack0 = link.keepalives_sent, link.keepalive_acks
    log_start = len(log.history)
    t_start = time.monotonic()
    next_poll = t_start
    rows = []
    writer_ctx = midi.Writer(pr["base"])
    try:
        while time.monotonic() - t_start < duration:
            t_cycle = time.monotonic()
            now = t_cycle - t_start
            row = dict(t=round(now, 1), cycle=c["cycles"])
            # --- PLAY z remote
            link.drain()
            remote.key("4", "0", "6", dbg.ENTER)
            t_key = time.monotonic()
            pkt = link.recv(2.5, from_remote(MT.PLAY_SONG))
            if pkt is None:
                c["play_missed"] += 1
                errors.append(f"{now:.0f}s: PLAY_SONG missing")
            else:
                play_lat.append(time.monotonic() - t_key)
                pc_ack(link, pkt)
                if pkt.payload != b"406":
                    errors.append(f"{now:.0f}s: PLAY_SONG payload {pkt.payload!r}")
            # --- harness -> remote
            ack_for(MT.SIGNAL_PLAYING, b"406A")
            for name in (b"Sloka 1\0", b"Sloka 2\0", b"Refren\0"):
                ack_for(MT.SIGNAL_ANCHOR, name)
            link.send(Packet(ADDR_PC, ADDR_BASE, 0, MT.NOW_PLAYING_INFO, bytes([0xF4, 0x06, 0xF2, 0x0A, 0x02])))
            try:
                writer_ctx.write(bytes([0x90, 0x3C, 0x64, 0x80, 0x3C, 0x00]))
            except OSError as e:
                c["midi_err"] += 1
                errors.append(f"{now:.0f}s: MIDI write {e}")
            time.sleep(0.4)
            ack_for(MT.SIGNAL_STOPPED)
            # opakovane PLAY_SONG (remote nedostal ACK) a zbytek provozu, ktery se nemel objevit
            time.sleep(0.2)
            extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype == MT.PLAY_SONG]
            c["play_retrans"] += len(extra)
            if extra:
                errors.append(f"{now:.0f}s: {len(extra)} PLAY_SONG retransmissions")
            # stav remote: cislice smazane?
            n = remote.u("GUI::playerScreen.digitCount_", 1)
            if n:
                c["digits_stale"] += 1
                remote.key(*([dbg.BACK] * n))
            # --- zdravi: jadra, spojeni
            for name, tgt in (("base", base), ("remote", remote)):
                st = tgt.core_state()
                if st["halted"]:
                    c["halted"] += 1
                    errors.append(f"{now:.0f}s: {name} core halted")
                if st["lockup"]:
                    c["lockup"] += 1
                    errors.append(f"{now:.0f}s: {name} core LOCKUP")
            if not base.read_bool("Bluetooth::_connected"):
                c["ble_disconnected"] += 1
            if not remote_alive(base):
                c["remote_dead"] += 1
                errors.append(f"{now:.0f}s: remote not alive")
            res = base.u("stmcpp::systick::resolution_")
            nowt = (res * base.u("stmcpp::systick::ticks_")) & 0xFFFFFFFF
            age = ((nowt - base.u("router::_lastRemoteRx")) & 0xFFFFFFFF) / res
            c["max_remote_age_ms"] = max(c["max_remote_age_ms"], age)
            # --- DIAG
            if time.monotonic() >= next_poll:
                next_poll += DIAG_PERIOD
                t_ack, t_resp, resp = diag_timed(link, cur)
                c["diag_polls"] += 1
                if resp is None:
                    if t_ack is None:
                        c["diag_no_ack"] += 1
                    else:
                        c["diag_no_resp"] += 1
                    errors.append(f"{now:.0f}s: DIAG poll failed (ack {t_ack}, nak/other {resp})")
                else:
                    lat_ack.append(t_ack)
                    lat_resp.append(t_resp)
                    row["diag_resp_s"] = round(t_resp, 3)
                    if t_resp > SLOW_S:
                        c["diag_slow"] += 1
                    bid, nxt, recs = decode(resp)
                    if bid != boot_id:
                        c["boot_changed"] += 1
                        errors.append(f"{now:.0f}s: remote boot_id {boot_id:#06x} -> {bid:#06x}")
                    bad = [x for x in recs if x[1] in (CODE_FAULT, CODE_LOOP_STALL)]
                    c["fault_recs"] += len(bad)
                    cur = max(cur, nxt)
            c["cycles"] += 1
            row.update({k: v for k, v in c.items() if k != "max_remote_age_ms"})
            row["remote_age_ms"] = round(age)
            rows.append(row)
            time.sleep(max(0.0, CYCLE - (time.monotonic() - t_cycle)))
    finally:
        writer_ctx.close()
        time.sleep(0.5)
        if rows:
            keys = sorted({k for r in rows for k in r}, key=lambda k: list(rows[0]).index(k) if k in rows[0] else 99)
            with open(os.path.join(out_dir, "soak.csv"), "w", newline="") as f:
                w = csv.DictWriter(f, keys, restval="")
                w.writeheader()
                w.writerows(rows)

    # --- vyhodnoceni
    time.sleep(1.0)
    ka_sent, ka_ack = link.keepalives_sent - ka_sent0, link.keepalive_acks - ka_ack0
    c["pc_keepalive_miss"] = ka_sent - ka_ack - 1  # posledni muze byt rozletany
    c["pc_keepalive_miss"] = max(0, c["pc_keepalive_miss"])
    hist = [l for _, l in log.history[log_start:]]
    log.close()
    parsed = [LOG_TS.match(l) for l in hist]
    lines_ew = [l for l, m in zip(hist, parsed) if m and m.group(3) in "EW" and not any(re.match(k, l) for k in KNOWN_LOG)]
    ts = [int(m.group(1)) * 1000 + int(m.group(2)) for m in parsed if m]
    markers = [l for l in hist if loglink.is_marker(l)]
    nonmono = sum(1 for a, b in zip(ts, ts[1:]) if b < a)
    banners = [l for l in hist if "MIDIControl base" in l and " I sys:" in l]
    c["log_lines"], c["log_ew"], c["log_nonmono"], c["log_markers"], c["log_banners"] = len(hist), len(lines_ew), nonmono, len(markers), len(banners)
    rec = base.fault_record()
    c["base_fault_record"] = 0 if rec == fault0 else 1  # novy/zmeneny zaznam od startu
    bid, nxt, recs = decode(diag_timed(link, 0xFFFF)[2])
    assert bid == boot_id
    st_pc_err = link.error
    stats = {}
    for name, v in (("diag_ack", lat_ack), ("diag_resp", lat_resp), ("play", play_lat)):
        if v:
            s = sorted(v)
            stats[name] = dict(n=len(v), min=s[0], med=statistics.median(s), p95=s[min(len(s) - 1, int(0.95 * len(s)))], max=s[-1])
    with open(os.path.join(out_dir, "summary.txt"), "w") as f:
        f.write(f"counters {c}\nlatency {stats}\nkeepalive PC sent {ka_sent} acked {ka_ack}\nerrors {errors[:40]}\nlog E/W {lines_ew[:40]}\n"
                f"markers {markers}\n")
    print(f"\nSOAK {duration:.0f} s: {c}\nlatency(s) {stats}\nPC keepalives sent {ka_sent} acked {ka_ack}\n"
          f"errors({len(errors)}): {errors[:10]}\nlog E/W: {lines_ew[:10]}")

    assert st_pc_err is None
    assert link.stray == b"", f"bytes outside frames on CDC: {link.stray!r}"
    zero = ["play_missed", "play_retrans", "ack_missed", "nak", "crc_err", "diag_no_ack", "diag_no_resp", "midi_err", "halted",
            "lockup", "remote_dead", "boot_changed", "fault_recs", "log_ew", "log_nonmono", "log_markers", "log_banners",
            "base_fault_record", "pc_keepalive_miss", "ble_disconnected"]
    bad = {k: c[k] for k in zero if c[k]}
    assert not bad, f"soak counters: {bad}; errors {errors[:10]}; log {lines_ew[:10]}"
    assert c["max_remote_age_ms"] < 6000, c["max_remote_age_ms"]
    assert remote_alive(base)
