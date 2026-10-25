"""Remote diagnostics via the base: DIAG_GET (0x80) -> ACK + DIAG_RESP (0x81), a ring of 32 records in RAM (Task 6D-1);
slots in the W25Q32, watchdogs and fault capture (Task 6D-2).
Precondition: the base and the remote are paired (test_pairing.py), the remote runs new firmware with diag.

A remote reset releases PA1 and the remote SWITCHES OFF (nobody switches it back on at the bench): tests that really trigger a fault/watchdog
are `needs_user` (they run only with `-m needs_user` and a person at the bench who switches the remote on). In practice a fault or
watchdog reset is over before the power latch drops and the remote stays on (only a long halt, e.g. a reflash, switches it off);
the tests accept both (warm reset: the RAM ring keeps the old run's records)."""

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

# A slot in the W25Q32 (midicontroller-fw src/logic/diag_slot.hpp): 256 B, sector 0x3FF000, 16 slots
SLOT_SIZE, SLOT_COUNT, SLOT_MAGIC = 256, 16, 0x544C5344
SLOT_EMPTY, SLOT_UNFETCHED, SLOT_FETCHED, SLOT_CORRUPT = range(4)
OFF_RECORDS, OFF_CRC, OFF_FETCHED = 48, 240, 255


def get(link, dst, cursor, timeout=2.0):
    """DIAG_GET -> (reply ACK/NAK, DIAG_RESP | None). DIAG_RESP additionally arrives after the ACK."""
    # a late DIAG_RESP from an earlier request (after a timeout) would otherwise be confused with the answer to this one
    link.drain()
    pid = link.send(Packet(ADDR_PC, dst, 0, MT.DIAG_GET, proto.diag_encode_get(cursor)))
    reply = link.recv(timeout, lambda p: p.src == dst and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    assert reply is not None, "no ACK/NAK for DIAG_GET"
    if reply.mtype != MT.ACK:
        return reply, None
    resp = link.recv(timeout, lambda p: p.src == dst and p.dst == ADDR_PC and p.mtype == MT.DIAG_RESP)
    if resp is not None:  # DIAG_RESP is acknowledged (PROTOCOL.md), otherwise the remote repeats it
        link.send(Packet(ADDR_PC, dst, 0, MT.ACK, bytes([resp.pid])))
    return reply, resp


def decode(resp):
    return proto.diag_decode_resp(resp.payload)


def decode_slot(raw: bytes) -> dict:
    """A slot (256 B) decoded independently of the firmware: magic, crc (zlib), header, registers, records (seq, code, level, uptime, arg)."""
    magic, boot_id, reason, count, uptime = struct.unpack_from("<IHBBI", raw, 0)
    regs = struct.unpack_from("<9I", raw, 12)
    crc = struct.unpack_from("<I", raw, OFF_CRC)[0]
    recs = [struct.unpack_from("<HBBII", raw, OFF_RECORDS + 12 * i) for i in range(min(count, 16))]
    return dict(magic=magic, boot_id=boot_id, reason=reason, count=count, uptime=uptime, regs=regs, recs=recs,
                crc_ok=crc == zlib.crc32(raw[:OFF_CRC]) & 0xFFFFFFFF, fetched=raw[OFF_FETCHED])


def head_and_boot(link):
    """(boot_id, head) from a DIAG_GET with the cursor past the end."""
    _, r = get(link, ADDR_REMOTE, 0xFFFF)
    assert r is not None, "no DIAG_RESP"
    boot_id, head, recs = decode(r)
    assert recs == []
    return boot_id, head


def read_range(link, start, end):
    """Records seq start..end-1 (paged DIAG_GET); returns (boot_id, records, last next)."""
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
    """A message to the person at the bench (bypassing pytest's capture)."""
    print(f"\n>>> {msg}", file=sys.__stderr__, flush=True)


@pytest.fixture(scope="module")
def paired(base):
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="BLE stream open (pair first: test_pairing.py)")
    wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
    return base


def test_diag_get_boot_record(paired, link):
    """BOOT is the first record of this run; before it there may be only records loaded from W25Q32 slots (at most 16)."""
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
    _, resp = get(link, ADDR_REMOTE, 0xFFFF)  # cursor past the end: n = 0, next = head
    boot_id, head0, recs = decode(resp)
    assert recs == [], recs
    def head_now():
        try:  # right after the stream opens the remote may not answer yet
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
        # every cycle: BLE_DISCONNECT, BLE_CONNECT, BLE_SECURED
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
    """60 s, DIAG_GET every 10 s with a full batch (8 records): exactly one ACK and one DIAG_RESP per GET, nothing
    extra (neither retries nor unsolicited), payload <= 101 B, wire frame <= 2x unescaped + 2 B.
    It is counted end-to-end via `link` (lossless); the logic analyzer is not used, its decoding of a 64 s recording
    dropped frames and the 120 B limit depends on the data (only 0x01/0x04/0x1B/'$'/'%' are escaped)."""
    remote = remote_probe

    def head_now():
        try:
            _, r = get(link, ADDR_REMOTE, 0xFFFF, timeout=1.0)
            return decode(r)[1] if r else -1
        except AssertionError:
            return -1

    while head_now() < 8:  # a supply of >= 8 records, so that a full batch is tried
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
            # nothing arrived in the meantime: no duplicate DIAG_RESP (retry) and no unsolicited frame from the remote
            extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype in (MT.DIAG_RESP, MT.ACK, MT.NAK)]
            assert extra == [], f"unsolicited/duplicate frames from remote after GET {i}: {extra}"
    gaps = [b - a for a, b in zip(sent_at, sent_at[1:])]
    assert all(g >= 9.5 for g in gaps), gaps
    time.sleep(2.0)
    extra = [p for p in link.drain() if p.src == ADDR_REMOTE and p.mtype in (MT.DIAG_RESP, MT.ACK, MT.NAK)]
    assert extra == [], extra
    print(f"6 GET -> 6 ACK + 6 DIAG_RESP, records per page {counts}, wire sizes {sizes}, gaps {[round(g, 1) for g in gaps]}")


def test_persisted_slot_roundtrip(paired, link, remote_probe):
    """A slot in the W25Q32 without a remote reset (reset = switch-off): gdb calls diag::testPersist() - the real persist() path
    as from a HardFault (interrupts disabled, SPI polling, one page program) with synthetic registers; the slot is read
    back into RAM (diag::testReadSlot -> w25q::read) and decoded independently (zlib crc). Then diag::testLoadSlot loads the slot
    into the ring as a boot, DIAG_GET shows its records and a cursor past them marks the slot as fetched (byte 255 = 0x00)."""
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
    # the slot records = the last records of this run, identical to the ring
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
    # a cursor past the last loaded record -> the program marks it "fetched"
    get(link, ADDR_REMOTE, nxt)
    wait_until(lambda: remote.call(f"diag::testReadSlot({idx})") == SLOT_FETCHED, 5, interval=0.5, what="slot marked fetched")
    raw = remote.read(remote.sym("diag::testBuf"), SLOT_SIZE)
    assert raw[OFF_FETCHED] == 0 and decode_slot(raw)["crc_ok"]
    print(f"slot {idx}: {slot['count']} records, boot_id {boot_id:#06x}, loaded as seq {head1}..{head1 + n - 1}, fetched")


def test_watchdogs_do_not_trip(paired, link, remote_probe):
    """10 min of normal operation (DIAG_GET every 10 s, PC -> remote relay, OLED via keys) + OTA staging erase
    (FW_UPDATE_START for the full size, then FW_UPDATE_END without data = BAD_SIZE: there is no abort message, the staging
    header stays erased) -> the remote never resets: same boot_id, seq grows, the link holds."""
    remote = remote_probe
    duration = int(os.environ.get("BASETEST_WDG_SECONDS", "600"))  # a shorter run for debugging only
    app_max = 0x0801F800 - 0x08004000
    boot_id, cur = head_and_boot(link)
    t0 = time.monotonic()

    # OTA erase (the longest blocking operation of the remote)
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
        # relay: PC -> remote (ACK from the remote)
        for mt, pl in ((MT.SIGNAL_PLAYING, b"406A"), (MT.SIGNAL_STOPPED, b"")):
            pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, mt, pl))
            r = link.recv(1.5, lambda p, pid=pid: p.src == ADDR_REMOTE and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
            assert r is not None and r.mtype == MT.ACK, f"{MT(mt).name}: {r}"
            signals += 1
        # OLED: a digit and its deletion (redraw without traffic on the link)
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


# TAMP->BKP1R on the remote (STM32G071): boot state, the low 16 bits count boots (diag::init); survives a warm reset
REMOTE_BKP1R = 0x4000B104


def _inject_and_wait_off(remote, paired, value: int, what: str):
    """Writes fault::testInject; returns the time [s] until the remote reset: the probe can no longer read (switched off) or the
    boot counter in BKP1R changed (a remote powered from USB/ST-Link stays on through the reset)."""
    bkp0 = remote.read_uint(REMOTE_BKP1R)
    t0 = time.monotonic()
    remote.write(remote.sym("fault::testInject"), bytes([value]))
    off, powered = None, False
    while time.monotonic() - t0 < 15:
        try:
            if remote.read_uint(REMOTE_BKP1R) != bkp0:
                off, powered = time.monotonic() - t0, True
                break
            if remote.core_state()["halted"]:
                remote.resume()
        except Exception:
            off = time.monotonic() - t0
            break
        time.sleep(0.05)
    assert off is not None, f"{what}: remote still running 15 s after the injection"
    remote.stop()  # openocd on a switched-off target
    if powered:
        print(f"{what}: remote reset after {off:.1f} s and stayed powered")
    else:
        tell_user(f"{what}: remote se vypnul ({off:.1f} s po injekci). Zapnete ho tlacitkem ON/OFF (do 3 min).")
    # The base notices the lost link only after the BLE supervision timeout: wait for the drop before waiting for the reconnect
    wait_until(lambda: not paired.read_bool("Bluetooth::_streamOpen"), 20, interval=0.2, what="base lost the old link")
    wait_until(lambda: paired.read_bool("Bluetooth::_streamOpen"), 180, interval=1.0, what="remote switched on and reconnected")
    wait_until(lambda: remote_alive(paired), 15, interval=0.5, what="remote alive")
    return off


def _records_before_boot(link):
    """Records before the newest BOOT back to the previous BOOT, and the newest BOOT. After a power-off these are the records
    loaded from slots; after a warm reset the RAM ring still holds the crashed run's own records there."""
    _, head = head_and_boot(link)
    _, recs, _ = read_range(link, 0, head)
    boots = [i for i, r in enumerate(recs) if r[1] == CODE_BOOT]
    assert boots, recs
    last = boots[-1]
    first = boots[-2] + 1 if len(boots) > 1 else 0
    return recs[first:last], recs[last]


@pytest.mark.needs_user
def test_remote_fault_survives_power_off(paired, link, remote_probe):
    """HardFault (fault::testInject = 1, unaligned read) -> FAULT(1) + 4x FAULT_DETAIL (pc in fault::testPoll) in the
    W25Q32 slot -> reset = switch-off; a person switches the remote on; the records are in the ring before BOOT."""
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
    """A loop with interrupts enabled (testInject = 2) -> after 2000 ms SysTick writes LOOP_STALL (pc in testSpin) + a slot
    -> reset = switch-off (~2 s); a person switches the remote on; LOOP_STALL is in the ring before BOOT."""
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
    """A loop with interrupts disabled (testInject = 3): SysTick does not run, the IWDG resets after ~4 s (LSI 29.5-34 kHz ->
    3.8-4.3 s). No slot (the IWDG gives no chance to write anything); after switch-on a BOOT, before it no new FAULT/LOOP_STALL.
    Reset cause: IWDG (bit 5) only if the remote survived the reset powered, otherwise POR."""
    remote = remote_probe
    # State before the injection: all FAULT/LOOP_STALL records known to this run (loaded from older slots and its own). After switch-on
    # only those may be before BOOT (re-loaded slots carry the original uptime and arg), no new one from the run ended by the IWDG.
    _, head0 = head_and_boot(link)
    _, pre, _ = read_range(link, 0, head0)
    off = _inject_and_wait_off(remote, paired, 3, "IWDG")
    assert 3.0 <= off <= 7.0, f"IWDG reset after {off:.1f} s, expected ~4 s"
    loaded, boot = _records_before_boot(link)
    print(f"IWDG: off after {off:.1f} s, BOOT cause {boot[4]:#04x}, records loaded from slots {len(loaded)}")
    new = _fault_keys(loaded) - _fault_keys(pre)
    assert not new, f"IWDG reset recorded as FAULT/LOOP_STALL (code, uptime, arg): {sorted(new)}; loaded {loaded}"


def _fault_keys(recs):
    """FAULT/LOOP_STALL records as (code, uptime_ms, arg): seq and boot change when loaded from a slot, these do not."""
    return {(r[1], r[3], r[4]) for r in recs if r[1] in (CODE_FAULT, CODE_LOOP_STALL)}
