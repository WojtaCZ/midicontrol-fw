"""Diagnostika remote pres base: DIAG_GET (0x80) -> ACK + DIAG_RESP (0x81), kruh 32 zaznamu v RAM (Task 6D-1);
sloty ve W25Q32, watchdogy a zachyceni chyb (Task 6D-2).
Predpoklad: base a remote jsou sparovane (test_pairing.py), remote bezi nove firmware s diag.

Reset remote uvolni PA1 a remote se VYPNE (nikdo ho na lavici nezapne): testy, ktere chybu/watchdog skutecne vyvolaji,
jsou `needs_user` (bezi jen s `-m needs_user` a clovekem u lavice, ktery remote zapne)."""

import os
import struct
import sys
import time
import zlib

import pytest

import dbg
import proto
from conftest import remote_alive, wait_until
from proto import MT, NAK, Packet

ADDR_PC, ADDR_BASE, ADDR_REMOTE = proto.ADDR_PC, proto.ADDR_BASE, proto.ADDR_REMOTE
CODE_BOOT = 1
CODE_FAULT = 2
CODE_FAULT_DETAIL = 3
CODE_LOOP_STALL = 4
CODE_RECORDS_LOST = 16
LEVEL_INFO = 3

# Slot ve W25Q32 (midicontroller-fw src/logic/diag_slot.hpp): 256 B, sektor 0x3FF000, 16 slotu
SLOT_SIZE, SLOT_COUNT, SLOT_MAGIC = 256, 16, 0x544C5344
SLOT_EMPTY, SLOT_UNFETCHED, SLOT_FETCHED, SLOT_CORRUPT = range(4)
OFF_RECORDS, OFF_CRC, OFF_FETCHED = 48, 240, 255


def get(link, dst, cursor, timeout=2.0):
    """DIAG_GET -> (reply ACK/NAK, DIAG_RESP | None). DIAG_RESP chodi navic po ACK."""
    # pozdni DIAG_RESP z drivejsiho pozadavku (po timeoutu) by se jinak spletla s odpovedi na tento
    link.drain()
    pid = link.send(Packet(ADDR_PC, dst, 0, MT.DIAG_GET, proto.diag_encode_get(cursor)))
    reply = link.recv(timeout, lambda p: p.src == dst and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    assert reply is not None, "no ACK/NAK for DIAG_GET"
    if reply.mtype != MT.ACK:
        return reply, None
    resp = link.recv(timeout, lambda p: p.src == dst and p.dst == ADDR_PC and p.mtype == MT.DIAG_RESP)
    if resp is not None:  # DIAG_RESP se potvrzuje (PROTOCOL.md), jinak ho remote opakuje
        link.send(Packet(ADDR_PC, dst, 0, MT.ACK, bytes([resp.pid])))
    return reply, resp


def decode(resp):
    return proto.diag_decode_resp(resp.payload)


def decode_slot(raw: bytes) -> dict:
    """Slot (256 B) nezavisle na firmware: magic, crc (zlib), hlavicka, registry, zaznamy (seq, code, level, uptime, arg)."""
    magic, boot_id, reason, count, uptime = struct.unpack_from("<IHBBI", raw, 0)
    regs = struct.unpack_from("<9I", raw, 12)
    crc = struct.unpack_from("<I", raw, OFF_CRC)[0]
    recs = [struct.unpack_from("<HBBII", raw, OFF_RECORDS + 12 * i) for i in range(min(count, 16))]
    return dict(magic=magic, boot_id=boot_id, reason=reason, count=count, uptime=uptime, regs=regs, recs=recs,
                crc_ok=crc == zlib.crc32(raw[:OFF_CRC]) & 0xFFFFFFFF, fetched=raw[OFF_FETCHED])


def head_and_boot(link):
    """(boot_id, head) z DIAG_GET s kurzorem za koncem."""
    _, r = get(link, ADDR_REMOTE, 0xFFFF)
    assert r is not None, "no DIAG_RESP"
    boot_id, head, recs = decode(r)
    assert recs == []
    return boot_id, head


def read_range(link, start, end):
    """Zaznamy seq start..end-1 (strankovane DIAG_GET); vraci (boot_id, zaznamy, posledni next)."""
    out, cur, bid = [], start, None
    while cur < end:
        _, r = get(link, ADDR_REMOTE, cur)
        assert r is not None, f"no DIAG_RESP for cursor {cur}"
        bid, nxt, recs = decode(r)
        assert recs, f"no records at cursor {cur} (head < {end}?)"
        out += recs
        cur = nxt
    return bid, out, cur


def tell_user(msg: str):
    """Zprava cloveku u lavice (mimo zachytavani pytest)."""
    print(f"\n>>> {msg}", file=sys.__stderr__, flush=True)


@pytest.fixture(scope="module")
def paired(base):
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="BLE stream open (pair first: test_pairing.py)")
    wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
    return base


def test_diag_get_boot_record(paired, link):
    """BOOT je prvni zaznam tohoto behu; pred nim smi byt jen zaznamy nactene ze slotu W25Q32 (nejvyse 16)."""
    link.drain()
    reply, resp = get(link, ADDR_REMOTE, 0)
    assert reply.mtype == MT.ACK, reply
    assert resp is not None, "no DIAG_RESP after ACK"
    boot_id, nxt, recs = decode(resp)
    assert len(recs) >= 1 and len(recs) <= proto.DIAG_MAX_RECORDS
    if recs[0][0] != 0:
        pytest.skip(f"ring wrapped (first seq {recs[0][0]}): reset the remote to see its BOOT record")
    while not any(r[1] == CODE_BOOT for r in recs) and nxt <= 17:
        _, more = read_range(link, nxt, nxt + 1)[:2]
        recs += more
        nxt = recs[-1][0] + 1
    boot = next((r for r in recs if r[1] == CODE_BOOT), None)
    assert boot is not None and boot[0] <= 16, recs
    seq, code, level, uptime, arg = boot
    assert level == LEVEL_INFO, boot
    print(f"boot_id {boot_id:#06x} BOOT seq {seq} (records loaded from slots before it: {seq}) reset cause {arg:#04x}")


def test_diag_cursor_paging(paired, link, remote_probe):
    remote = remote_probe
    link.drain()
    _, resp = get(link, ADDR_REMOTE, 0xFFFF)  # kurzor za koncem: n = 0, next = head
    boot_id, head0, recs = decode(resp)
    assert recs == [], recs
    def head_now():
        try:  # hned po otevreni streamu remote jeste nemusi odpovidat
            _, r = get(link, ADDR_REMOTE, 0xFFFF, timeout=1.0)
            return decode(r)[1] if r else -1
        except AssertionError:
            return -1

    cycles, head = 0, head0
    while head - head0 < 10 and cycles < 5:
        remote.hold_ble_reset(True)
        wait_until(lambda: not paired.read_bool("Bluetooth::_streamOpen"), 10, what="base sees disconnect")
        remote.hold_ble_reset(False)
        wait_until(lambda: paired.read_bool("Bluetooth::_streamOpen"), 15, what="reconnected")
        cycles += 1
        # kazdy cyklus: BLE_DISCONNECT, BLE_CONNECT, BLE_SECURED
        wait_until(lambda: head_now() >= head + 3, 15, interval=0.5, what="records of the reset cycle")
        head = head_now()
    assert head - head0 >= 10, f"only {head - head0} records after {cycles} reset cycles"

    cur, seen, pages = head0, [], 0
    while cur < head:
        _, resp = get(link, ADDR_REMOTE, cur)
        bid, nxt, recs = decode(resp)
        assert bid == boot_id, "boot_id changed without a remote reset"
        assert 0 < len(recs) <= proto.DIAG_MAX_RECORDS
        seen += [r[0] for r in recs]
        assert nxt == recs[-1][0] + 1
        cur, pages = nxt, pages + 1
    assert seen == list(range(head0, head)), seen
    assert pages >= 2
    _, resp = get(link, ADDR_REMOTE, cur)
    assert decode(resp)[2] == [], "records past the head"
    print(f"{cycles} reset cycles, records {head0}..{head - 1} in {pages} pages")


def test_diag_get_to_base_nak(paired, link):
    link.drain()
    pid = link.send(Packet(ADDR_PC, ADDR_BASE, 0, MT.DIAG_GET, proto.diag_encode_get(0)))
    r = link.recv(1.5, lambda p: p.src == ADDR_BASE and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    assert r is not None and r.mtype == MT.NAK and r.payload == bytes([pid, NAK.UNKNOWN_MSG]), r


def test_diag_traffic_budget(paired, link, remote_probe):
    """60 s, DIAG_GET kazdych 10 s s plnou davkou (8 zaznamu): presne jeden ACK a jedna DIAG_RESP na GET, nic
    navic (ani opakovani, ani neziadane), payload <= 101 B, ramec na drate <= 2x nezeskapovany + 2 B.
    Pocita se end-to-end pres `link` (bez ztrat); logicky analyzator se nepouziva, jeho dekodovani 64 s zaznamu
    ztracelo ramce a mez 120 B zavisi na datech (escapuji se jen 0x01/0x04/0x1B/'$'/'%')."""
    remote = remote_probe

    def head_now():
        try:
            _, r = get(link, ADDR_REMOTE, 0xFFFF, timeout=1.0)
            return decode(r)[1] if r else -1
        except AssertionError:
            return -1

    while head_now() < 8:  # zasoba >= 8 zaznamu, aby se zkusila plna davka
        remote.hold_ble_reset(True)
        wait_until(lambda: not paired.read_bool("Bluetooth::_streamOpen"), 10, what="base sees disconnect")
        remote.hold_ble_reset(False)
        wait_until(lambda: paired.read_bool("Bluetooth::_streamOpen"), 15, what="reconnected")
        time.sleep(3.0)
    time.sleep(2.0)
    link.drain()

    sent_at, sizes, counts = [], [], []
    cursor = 0
    t0 = time.monotonic()
    for i in range(6):
        sent_at.append(time.monotonic())
        reply, resp = get(link, ADDR_REMOTE, cursor)
        assert reply.mtype == MT.ACK and resp is not None, (i, reply)
        assert len(resp.payload) <= proto.DIAG_RESP_MAX_PAYLOAD, len(resp.payload)
        unescaped = len(proto.raw_body(resp.src, resp.dst, resp.pid, resp.mtype, resp.payload))
        wire = len(proto.encode_packet(resp))
        assert wire <= 2 * unescaped + 2, (wire, unescaped)
        sizes.append(wire)
        counts.append(len(decode(resp)[2]))
        if i == 0:
            assert counts[0] == proto.DIAG_MAX_RECORDS, f"first page has {counts[0]} records, wanted the full 8"
        cursor = decode(resp)[1]
        if i < 5:
            time.sleep(max(0.0, t0 + 10 * (i + 1) - time.monotonic()))
            # mezitim nic nepriteklo: zadny duplikat DIAG_RESP (retry) ani neziadany ramec od remote
            extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype in (MT.DIAG_RESP, MT.ACK, MT.NAK)]
            assert extra == [], f"unsolicited/duplicate frames from remote after GET {i}: {extra}"
    gaps = [b - a for a, b in zip(sent_at, sent_at[1:])]
    assert all(g >= 9.5 for g in gaps), gaps
    time.sleep(2.0)
    extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype in (MT.DIAG_RESP, MT.ACK, MT.NAK)]
    assert extra == [], extra
    print(f"6 GET -> 6 ACK + 6 DIAG_RESP, records per page {counts}, wire sizes {sizes}, gaps {[round(g, 1) for g in gaps]}")


def test_persisted_slot_roundtrip(paired, link, remote_probe):
    """Slot ve W25Q32 bez resetu remote (reset = vypnuti): gdb zavola diag::testPersist() - skutecna cesta persist()
    jako z HardFaultu (preruseni zakazana, polling SPI, jeden page program) se syntetickymi registry; slot se precte
    zpet do RAM (diag::testReadSlot -> w25q::read) a dekoduje nezavisle (zlib crc). Pak diag::testLoadSlot nacte slot
    do kruhu jako boot, DIAG_GET jeho zaznamy ukaze a kurzor za nimi slot oznaci jako vyzvednuty (bajt 255 = 0x00)."""
    remote = remote_probe
    marker = 0x6D200000
    boot_id, head0 = head_and_boot(link)
    idx = remote.call(f"diag::testPersist({marker:#x})")
    assert 0 <= idx < SLOT_COUNT, f"testPersist -> {idx} (sector full this boot or W25Q32 not responding)"
    assert remote.call(f"diag::testReadSlot({idx})") == SLOT_UNFETCHED
    slot = decode_slot(remote.read(remote.sym("diag::testBuf"), SLOT_SIZE))
    assert slot["magic"] == SLOT_MAGIC and slot["crc_ok"], slot
    assert slot["boot_id"] == boot_id and slot["reason"] == CODE_FAULT, slot
    assert slot["regs"] == tuple(marker + i for i in range(9)), [hex(r) for r in slot["regs"]]
    assert slot["fetched"] == 0xFF
    assert 1 <= slot["count"] <= 16, slot
    # zaznamy slotu = posledni zaznamy tohoto behu, shodne s kruhem
    first = slot["recs"][0][0]
    _, ring, _ = read_range(link, first, first + slot["count"])
    assert ring[: slot["count"]] == slot["recs"], (ring, slot["recs"])

    _, head1 = head_and_boot(link)
    n = remote.call(f"diag::testLoadSlot({idx})")
    assert n == slot["count"], n
    bid, loaded, nxt = read_range(link, head1, head1 + n)
    assert bid == boot_id, "remote reset during the test"
    loaded = loaded[:n]
    assert [r[0] for r in loaded] == list(range(head1, head1 + n))
    assert [r[1:] for r in loaded] == [r[1:] for r in slot["recs"]], (loaded, slot["recs"])
    # kurzor za poslednim nactenym zaznamem -> program "fetched"
    get(link, ADDR_REMOTE, nxt)
    wait_until(lambda: remote.call(f"diag::testReadSlot({idx})") == SLOT_FETCHED, 5, interval=0.5, what="slot marked fetched")
    raw = remote.read(remote.sym("diag::testBuf"), SLOT_SIZE)
    assert raw[OFF_FETCHED] == 0 and decode_slot(raw)["crc_ok"]
    print(f"slot {idx}: {slot['count']} records, boot_id {boot_id:#06x}, loaded as seq {head1}..{head1 + n - 1}, fetched")


def test_watchdogs_do_not_trip(paired, link, remote_probe):
    """10 min bezneho provozu (DIAG_GET kazdych 10 s, relay PC -> remote, OLED pres klavesy) + mazani OTA stagingu
    (FW_UPDATE_START na plnou velikost, pak FW_UPDATE_END bez dat = BAD_SIZE: zadne abort zpravy neni, hlavicka
    stagingu zustava smazana) -> remote se nikdy neresetuje: boot_id stejny, seq roste, spojeni drzi."""
    remote = remote_probe
    duration = int(os.environ.get("BASETEST_WDG_SECONDS", "600"))  # kratsi beh jen pro ladeni
    app_max = 0x0801F800 - 0x08004000
    boot_id, cur = head_and_boot(link)
    t0 = time.monotonic()

    # OTA erase (nejdelsi blokujici operace remote)
    link.drain()
    pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.FW_UPDATE_START, struct.pack("<BII", ADDR_REMOTE, app_max, 0x12345678) + b"wdg"))
    r = link.recv(15.0, lambda p: p.src == ADDR_REMOTE and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    erase_s = time.monotonic() - t0
    assert r is not None and r.mtype == MT.ACK, f"FW_UPDATE_START: {r}"
    st = link.recv(2.0, lambda p: p.src == ADDR_REMOTE and p.mtype == MT.FW_UPDATE_STATUS)
    assert st is not None and st.payload[:1] == b"\x00", st  # READY
    pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.FW_UPDATE_END, b""))
    r = link.recv(3.0, lambda p: p.src == ADDR_REMOTE and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    assert r is not None and r.mtype == MT.NAK, f"FW_UPDATE_END without data: {r}"
    link.recv(2.0, lambda p: p.src == ADDR_REMOTE and p.mtype == MT.FW_UPDATE_STATUS)  # BAD_SIZE

    remote.goto_player()
    polls = keys = signals = 0
    next_poll = time.monotonic()
    while time.monotonic() - t0 < duration:
        if time.monotonic() >= next_poll:
            _, resp = get(link, ADDR_REMOTE, cur)
            assert resp is not None, f"no DIAG_RESP at {time.monotonic() - t0:.0f} s"
            bid, nxt, recs = decode(resp)
            assert bid == boot_id, f"remote reset (boot_id {boot_id:#06x} -> {bid:#06x}) at {time.monotonic() - t0:.0f} s"
            assert nxt >= cur
            bad = [x for x in recs if x[1] in (CODE_FAULT, CODE_LOOP_STALL)]
            assert not bad, bad
            cur, polls = nxt, polls + 1
            next_poll += 10
        # relay: PC -> remote (ACK od remote)
        for mt, pl in ((MT.SIGNAL_PLAYING, b"406A"), (MT.SIGNAL_STOPPED, b"")):
            pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, mt, pl))
            r = link.recv(1.5, lambda p, pid=pid: p.src == ADDR_REMOTE and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
            assert r is not None and r.mtype == MT.ACK, f"{MT(mt).name}: {r}"
            signals += 1
        # OLED: cislice a jeji smazani (prekresleni bez provozu na lince)
        remote.key("1", dbg.BACK)
        keys += 2
        assert not remote.core_state()["halted"], "remote core left halted"
        time.sleep(3.0)
    assert remote_alive(paired), "remote not alive at the end"
    bid, head = head_and_boot(link)
    assert bid == boot_id
    gap, boot_ms = remote.u("wdg::maxGapMs"), remote.u("wdg::bootToLoopMs")
    assert gap < 2000, f"max gap between kicks {gap} ms"
    print(f"{duration} s: {polls} DIAG polls, {signals} relayed signals, {keys} keys, OTA erase ACK after {erase_s:.2f} s, "
          f"boot_id {boot_id:#06x} unchanged, head {head}, max kick gap {gap} ms, boot-to-loop {boot_ms} ms")


def _inject_and_wait_off(remote, paired, value: int, what: str):
    """Zapise fault::testInject; vraci cas [s] do ztraty remote (sonda uz necte = vypnuto)."""
    t0 = time.monotonic()
    remote.write(remote.sym("fault::testInject"), bytes([value]))
    off = None
    while time.monotonic() - t0 < 15:
        try:
            remote.tcl("read_memory 0x20000000 32 1")
            if remote.core_state()["halted"]:
                remote.resume()
        except Exception:
            off = time.monotonic() - t0
            break
        if not paired.read_bool("Bluetooth::_streamOpen") and off is None:
            off = time.monotonic() - t0
            break
        time.sleep(0.1)
    assert off is not None, f"{what}: remote still running 15 s after the injection"
    remote.stop()  # openocd na vypnutem cili
    tell_user(f"{what}: remote se vypnul ({off:.1f} s po injekci). Zapnete ho tlacitkem ON/OFF (do 3 min).")
    wait_until(lambda: paired.read_bool("Bluetooth::_streamOpen"), 180, interval=1.0, what="remote switched on and reconnected")
    wait_until(lambda: remote_alive(paired), 15, interval=0.5, what="remote alive")
    return off


def _records_before_boot(link):
    """Zaznamy nactene ze slotu (pred BOOT) a BOOT."""
    _, recs, _ = read_range(link, 0, 1)
    nxt = recs[-1][0] + 1
    while not any(r[1] == CODE_BOOT for r in recs):
        _, more, nxt = read_range(link, nxt, nxt + 1)
        recs += more
    i = next(i for i, r in enumerate(recs) if r[1] == CODE_BOOT)
    return recs[:i], recs[i]


@pytest.mark.needs_user
def test_remote_fault_survives_power_off(paired, link, remote_probe):
    """HardFault (fault::testInject = 1, nezarovnane cteni) -> FAULT(1) + 4x FAULT_DETAIL (pc v fault::testPoll) ve
    slotu W25Q32 -> reset = vypnuti; clovek remote zapne; zaznamy jsou v kruhu pred BOOT."""
    remote = remote_probe
    boot0, _ = head_and_boot(link)
    _inject_and_wait_off(remote, paired, 1, "HardFault")
    loaded, boot = _records_before_boot(link)
    bid, _ = head_and_boot(link)
    assert bid != boot0
    codes = [r[1] for r in loaded]
    assert CODE_FAULT in codes, loaded
    i = codes.index(CODE_FAULT)
    assert loaded[i][4] == 1 and codes[i + 1 : i + 5] == [CODE_FAULT_DETAIL] * 4, loaded
    pc = loaded[i + 1][4]
    assert "testPoll" in remote.addr2line(pc), (hex(pc), remote.addr2line(pc))
    print(f"FAULT pc {pc:#x} ({remote.addr2line(pc)}), BOOT cause {boot[4]:#04x}")


@pytest.mark.needs_user
def test_remote_loop_stall(paired, link, remote_probe):
    """Smycka s povolenymi prerusenimi (testInject = 2) -> po 2000 ms SysTick zapise LOOP_STALL(pc v testSpin) + slot
    -> reset = vypnuti (~2 s); clovek remote zapne; LOOP_STALL je v kruhu pred BOOT."""
    remote = remote_probe
    off = _inject_and_wait_off(remote, paired, 2, "loop stall")
    assert off >= 1.8, f"reset after {off:.1f} s, loop watchdog is 2000 ms"
    loaded, boot = _records_before_boot(link)
    stall = [r for r in loaded if r[1] == CODE_LOOP_STALL]
    assert stall, loaded
    pc = stall[-1][4]
    assert "testSpin" in remote.addr2line(pc), (hex(pc), remote.addr2line(pc))
    print(f"LOOP_STALL pc {pc:#x} ({remote.addr2line(pc)}) after {off:.1f} s")


@pytest.mark.needs_user
def test_remote_iwdg(paired, link, remote_probe):
    """Smycka se zakazanymi prerusenimi (testInject = 3): SysTick nebezi, resetuje IWDG po ~4 s (LSI 29,5-34 kHz ->
    3,8-4,3 s). Zadny slot (IWDG nedava sanci nic zapsat); po zapnuti BOOT, pred nim zadny novy FAULT/LOOP_STALL.
    Pricina resetu: IWDG (bit 5) jen kdyz remote reset prezil napajeny, jinak POR."""
    remote = remote_probe
    off = _inject_and_wait_off(remote, paired, 3, "IWDG")
    assert 3.0 <= off <= 7.0, f"IWDG reset after {off:.1f} s, expected ~4 s"
    loaded, boot = _records_before_boot(link)
    print(f"IWDG: off after {off:.1f} s, BOOT cause {boot[4]:#04x}, records loaded from slots {len(loaded)}")
