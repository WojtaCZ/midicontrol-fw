"""Soak celeho systemu (Task 7C): remote (klavesy pres ST-Link) <-> base <-> PC harness, `SOAK_SECONDS` (default 600).
Cyklus (~3 s): PLAY z remote (klavesy 406 + ENTER) -> harness potvrdi, SIGNAL_PLAYING, 3x SIGNAL_ANCHOR, NOW_PLAYING_INFO,
USB-MIDI noty na base (DIN nelze overit, bench DIN link je rozbity: jen se pozoruje, ze zapis projde a log je cisty),
SIGNAL_STOPPED. Kazdych 10 s DIAG_GET na remote s merenim latence DIAG_GET -> DIAG_RESP (R28). Pocitadla po iteracich
jdou do CSV (`SOAK_OUT`, default <tmp>/basetest-soak/soak.csv), na konci se vypise souhrn.
CRC: base BLE strana se NEMERI (bluetooth.cpp nema pocitadlo); CDC: router::_badFrames se po hlaseni nuluje, chyby prichazi
jako W radek "cdc: N bad frames dropped" (CRC, delka i neuplne ramce dohromady; log ma kladnou kontrolu); remote: zaznamy CRC_ERRORS z DIAG. Jediny ztraceny DIAG poll soak shodi (zamer).
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
CODE_CRC_ERRORS = 11  # diag zaznam remote: CRC chyby na BLE lince
MAX_REMOTE_AGE_MS = 3000  # base -> remote keepalive po 2 s (+ rezerva); 6000 ms je az prah ztraty spojeni
LOG_TS = re.compile(r"^\[ *(\d+)\.(\d{3})\] ([EWID]) ([a-z]{1,6}): (.*)$")


@pytest.fixture(scope="module")
def paired(base):
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="BLE stream open (pair first: test_pairing.py)")
    wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
    return base


def diag_timed(link, cursor, rv=None):
    """DIAG_GET -> (ack_s, resp_s, resp); None kdyz chybi. Cas DIAG_RESP od odeslani DIAG_GET (do 8 s, bez opakovani)."""
    link.drain()
    t0 = time.monotonic()
    pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.DIAG_GET, proto.diag_encode_get(cursor)))
    rv = rv or link.recv
    ack = rv(8.0, reply_to(pid))
    t_ack = time.monotonic() - t0
    if ack is None or ack.mtype != MT.ACK:
        return None, None, ack
    resp = rv(8.0 - t_ack, lambda p: p.src == ADDR_REMOTE and p.dst == ADDR_PC and p.mtype == MT.DIAG_RESP)
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
    c = dict(cycles=0, elapsed_s=0.0, max_key_s=0.0, max_sample_gap_ms=0.0, age_samples=0, base_reset=0, log_alive=0, remote_crc_recs=0, play_missed=0, play_retrans=0, ack_missed=0, nak=0, diag_polls=0, diag_no_ack=0,
             diag_no_resp=0, diag_nak=0, diag_slow=0, midi_err=0, halted=0, lockup=0, remote_dead=0, digits_stale=0,
             fault_recs=0, boot_changed=0, ble_disconnected=0, max_remote_age_ms=0.0)
    lat_ack, lat_resp = [], []
    errors = []

    # okno mereni = smycka cyklu: t_start se prepise tesne pred ni; mezery mezi vzorky a jejich pocet se pocitaji jen v okne,
    # stari remote a systick se kontroluji u kazdeho vzorku (i v priprave pred smyckou)
    t_start = time.monotonic()
    win = dict(on=False, last=None)
    last_ticks = None
    last_age = [0.0]

    def sample():
        """Systick (reset detekce) a stari posledniho paketu od remote; v okne se vola >= 4x/s i behem cekani cyklu."""
        nonlocal last_ticks
        tnow = time.monotonic()
        if win["on"]:
            if win["last"] is not None:
                c["max_sample_gap_ms"] = max(c["max_sample_gap_ms"], (tnow - win["last"]) * 1000)
            win["last"] = tnow
            c["age_samples"] += 1
        res = base.u("stmcpp::systick::resolution_")
        ticks = base.u("stmcpp::systick::ticks_")
        if last_ticks is not None and ticks < last_ticks:
            c["base_reset"] += 1
            errors.append(f"{time.monotonic() - t_start:.0f}s: base systick went back ({last_ticks} -> {ticks}): reset")
        last_ticks = ticks
        nowt = (res * ticks) & 0xFFFFFFFF
        age = ((nowt - base.u("router::_lastRemoteRx")) & 0xFFFFFFFF) / res
        last_age[0] = age
        c["max_remote_age_ms"] = max(c["max_remote_age_ms"], age)

    def recv_s(timeout, pred=None):
        """link.recv po <= 0.2 s dilech, mezi nimi sample(): cekani na odpoved nezakryje ztracene keepalive."""
        end = time.monotonic() + timeout
        while True:
            sample()
            left = end - time.monotonic()
            if left <= 0:
                return None
            p = link.recv(min(0.2, left), pred)
            if p is not None:
                return p

    def nap(seconds):
        end = time.monotonic() + seconds
        while True:
            sample()
            left = end - time.monotonic()
            if left <= 0:
                return
            time.sleep(min(0.2, left))

    def key_timed(row, col, *keys):
        """remote.key() je gdb batch (nejde rozdelit): kazde volani se meri (CSV sloupec col, max_key_s)."""
        tk = time.monotonic()
        remote.key(*keys)
        d = time.monotonic() - tk
        row[col] = round(d, 3)
        c["max_key_s"] = max(c["max_key_s"], round(d, 3))

    def ack_for(mtype, payload=b""):
        pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, mtype, payload))
        r = recv_s(1.5, reply_to(pid))
        if r is None:
            c["ack_missed"] += 1
            errors.append(f"{time.monotonic() - t_start:.0f}s: no ACK for {MT(mtype).name}")
        elif r.mtype != MT.ACK:
            c["nak"] += 1
            errors.append(f"{time.monotonic() - t_start:.0f}s: NAK for {MT(mtype).name}: {r}")

    remote.goto_player()
    ack_for(MT.SIGNAL_STOPPED)
    n = remote.u("GUI::playerScreen.digitCount_", 1) + (1 if remote.u("GUI::playerScreen.letter_", 1) else 0)
    if n:
        remote.key(*([dbg.BACK] * n))
    link.drain()
    # kladna kontrola logu: bez ni by "zadne E/W" nic neznamenalo (default uroven je tiha)
    # vychozi uroven je INFO (src/debug.cpp), po kontrole se vraci `level i`
    try:
        assert log.command("level d") == ["level D"], "log CDC not answering"
        link.send_msg(MT.GET_STATUS)
        got = log.lines(3.0, pred=lambda l: loglink.is_log_line(l) and " D " in l)
        assert any(loglink.is_log_line(l) and " D " in l for l in got), "no D line on the log CDC at level d"
    finally:
        assert log.command("level i") == ["level I"]
    c["log_alive"] = 1
    log.skip()
    link.drain()
    fault0 = base.fault_record()  # zbytek z drivejsich testu (.noinit): nesmi se zmenit
    # pocatecni stav
    t_ack, t_resp, resp = diag_timed(link, 0xFFFF)
    assert resp is not None, "no DIAG_RESP at start"
    boot_id, cur, _ = decode(resp)
    ka_sent0, ka_ack0 = link.keepalives_sent, link.keepalive_acks
    log_start = len(log.history)
    rows = []
    writer_ctx = midi.Writer(pr["base"])
    t_start = time.monotonic()
    next_poll = t_start
    win["on"] = True
    sample()  # prvni vzorek okna: i prvni remote.key() je mezi dvema vzorky
    try:
        while time.monotonic() - t_start < duration:
            t_cycle = time.monotonic()
            now = t_cycle - t_start
            row = dict(t=round(now, 1), cycle=c["cycles"])
            # --- PLAY z remote
            link.drain()
            key_timed(row, "key_s", "4", "0", "6", dbg.ENTER)
            pkt = recv_s(2.5, from_remote(MT.PLAY_SONG))
            if pkt is None:
                c["play_missed"] += 1
                errors.append(f"{now:.0f}s: PLAY_SONG missing")
            else:
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
            nap(0.4)
            ack_for(MT.SIGNAL_STOPPED)
            # opakovane PLAY_SONG (remote nedostal ACK) a zbytek provozu, ktery se nemel objevit
            nap(0.2)
            extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype == MT.PLAY_SONG]
            c["play_retrans"] += len(extra)
            if extra:
                errors.append(f"{now:.0f}s: {len(extra)} PLAY_SONG retransmissions")
            # stav remote: cislice smazane?
            n = remote.u("GUI::playerScreen.digitCount_", 1)
            if n:
                c["digits_stale"] += 1
                key_timed(row, "back_key_s", *([dbg.BACK] * n))
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
            sample()
            age = last_age[0]
            # --- DIAG
            if time.monotonic() >= next_poll:
                next_poll += DIAG_PERIOD
                t_ack, t_resp, resp = diag_timed(link, cur, recv_s)
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
                    c["remote_crc_recs"] += sum(1 for x in recs if x[1] == CODE_CRC_ERRORS)
                    cur = max(cur, nxt)
            c["cycles"] += 1
            row.update({k: v for k, v in c.items() if k != "max_remote_age_ms"})
            row["remote_age_ms"] = round(age)
            rows.append(row)
            nap(max(0.0, CYCLE - (time.monotonic() - t_cycle)))
    finally:
        c["elapsed_s"] = round(time.monotonic() - t_start, 2)  # posledni nap() konci vzorkem
        win["on"] = False
        writer_ctx.close()
        time.sleep(0.5)
        if rows:
            keys = sorted({k for r in rows for k in r}, key=lambda k: list(rows[0]).index(k) if k in rows[0] else 99)
            with open(os.path.join(out_dir, "soak.csv"), "w", newline="") as f:
                w = csv.DictWriter(f, keys, restval="")
                w.writeheader()
                w.writerows(rows)

    # --- vyhodnoceni
    ka_sent = link.keepalives_sent - ka_sent0
    wait_end = time.monotonic() + 1.5  # posledni keepalive dostane cas na ACK
    while time.monotonic() < wait_end and link.keepalive_acks - ka_ack0 < ka_sent:
        time.sleep(0.05)
    ka_ack = link.keepalive_acks - ka_ack0
    c["pc_keepalive_miss"] = ka_sent - ka_ack
    hist = [l for _, l in log.history[log_start:]]
    log.close()
    parsed = [LOG_TS.match(l) for l in hist]
    lines_ew = [l for l, m in zip(hist, parsed) if m and m.group(3) in "EW"]
    ts = [int(m.group(1)) * 1000 + int(m.group(2)) for m in parsed if m]
    markers = [l for l in hist if loglink.is_marker(l)]
    nonmono = sum(1 for a, b in zip(ts, ts[1:]) if b < a)
    banners = [l for l in hist if "MIDIControl base" in l and " I sys:" in l]
    c["log_lines"], c["log_ew"], c["log_nonmono"], c["log_markers"], c["log_banners"] = len(hist), len(lines_ew), nonmono, len(markers), len(banners)
    rec = base.fault_record()
    c["base_fault_record"] = 0 if rec == fault0 else 1  # novy/zmeneny zaznam od startu
    _, _, final = diag_timed(link, 0xFFFF)
    assert final is not None, "no DIAG_RESP at the end"
    assert decode(final)[0] == boot_id
    st_pc_err = link.error
    stats = {}
    for name, v in (("diag_ack", lat_ack), ("diag_resp", lat_resp)):
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
    zero = ["play_missed", "play_retrans", "ack_missed", "nak", "base_reset", "remote_crc_recs", "diag_no_ack", "diag_no_resp", "midi_err", "halted",
            "lockup", "remote_dead", "boot_changed", "fault_recs", "log_ew", "log_nonmono", "log_markers", "log_banners",
            "base_fault_record", "pc_keepalive_miss", "ble_disconnected"]
    bad = {k: c[k] for k in zero if c[k]}
    assert not bad, f"soak counters: {bad}; errors {errors[:10]}; log {lines_ew[:10]}"
    assert c["log_alive"]
    assert c["max_sample_gap_ms"] < 1000, f"age sampling gap {c['max_sample_gap_ms']:.0f} ms (remote.key max {c['max_key_s']} s): a lost keepalive could hide"
    assert c["age_samples"] >= 4 * c["elapsed_s"], f"{c['age_samples']} age samples in {c['elapsed_s']} s of loop (< 4 Hz)"
    assert c["max_remote_age_ms"] < MAX_REMOTE_AGE_MS, f"max remote age {c['max_remote_age_ms']:.0f} ms >= {MAX_REMOTE_AGE_MS}"
    assert remote_alive(base)
