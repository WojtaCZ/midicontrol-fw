"""Odolnost bootloaderu base (plan docs/superpowers/plans/2026-10-24-bootloader-robustness-tests.md, Tasks 2-4).

Kazdy test konci s base v aplikaci build/midicontrol-fw-update.bin; zustane-li v bootloaderu, fixture `recover` ji
nahraje PC updaterem (produktova cesta obnovy, BASETEST_UPDATER) nebo fwclientem; visi-li (NMI/HardFault/lockup),
obnovi se SWD a test selze. Harness link (session `link`) se zde nepouziva: kazdy test si otevira port sam."""

import os
import re
import subprocess
import threading
import time
import zlib

import pytest

import bootfi
import fwclient
import proto
import usblink
from conftest import wait_until
from proto import MT, Packet

UPDATER = os.environ.get("BASETEST_UPDATER", "")
IMG = open(bootfi.BOARDS["base"]["update_bin"], "rb").read()
APP = bootfi.BOARDS["base"]["app"]
APP_MAX = bootfi.BOARDS["base"]["app_end"] - APP


def usb_ids() -> set:
    return set(re.findall(r"ID (abcd:123[45])", subprocess.run(["lsusb"], capture_output=True, text=True).stdout))


def usb_devnum(pid: str):
    for d in os.listdir("/sys/bus/usb/devices"):
        p = f"/sys/bus/usb/devices/{d}"
        if usblink._read(f"{p}/idVendor") == "abcd" and usblink._read(f"{p}/idProduct") == pid:
            return int(usblink._read(f"{p}/devnum"))
    return None


def wait_ids(want: str, timeout=10.0):
    wait_until(lambda: usb_ids() == {want}, timeout, interval=0.1, what=f"only {want} on USB (now {usb_ids()})")


def enter_bootloader() -> usblink.Link:
    """App -> ENTER_BOOTLOADER -> Link na portu bootloaderu. Je-li base uz v bootloaderu (predchozi beh), nejdriv dobry
    obraz."""
    if usb_ids() == {"abcd:1235"}:
        assert flash_good() == "ok"
    wait_ids("abcd:1234", 15)
    L = usblink.Link(port=usblink.find_port())
    L.send(Packet(proto.ADDR_PC, proto.ADDR_BASE, 0, MT.ENTER_BOOTLOADER, b""))
    L.recv(1.0, lambda p: p.mtype in (MT.ACK, MT.NAK))
    L.close()
    wait_ids("abcd:1235", 10)
    return usblink.Link(port=fwclient.bootloader_port())


def bl_link() -> usblink.Link:
    wait_ids("abcd:1235", 10)
    return usblink.Link(port=fwclient.bootloader_port())


def flash_good(link=None) -> str:
    """Dobry obraz do bootloaderu fwclientem; ceka na aplikaci."""
    L = link or bl_link()
    try:
        res = fwclient.Client(L, proto.ADDR_BASE).send_image(IMG)
    finally:
        L.close()
    if res == "ok":
        wait_ids("abcd:1234", 15)
    return res


def pc_updater_recover():
    """Base v bootloaderu: PC updater (produkt) ji obnovi."""
    assert UPDATER and os.path.exists(UPDATER), "BASETEST_UPDATER required"
    from test_ota import Updater
    up = Updater("base", bootfi.BOARDS["base"]["update_bin"], "base-bootrobust-recover")
    try:
        rc = up.wait(300)
    finally:
        up.kill()
    assert rc == 0, up.text()[-600:]
    wait_ids("abcd:1234", 20)


@pytest.fixture
def board():
    b = bootfi.Board("base")
    yield b
    b.stop()


def base_healthy(board):
    """(ok, popis): jadro v aplikaci, oblast = build, jen abcd:1234 na USB."""
    try:
        w = board.where()
        matches = board.app_matches()
    except Exception as e:  # noqa: BLE001 - sonda / jadro nedostupne
        return False, f"probe error {e!r}"
    finally:
        board.stop()
    ids = usb_ids()
    ok = w["kind"] == "app" and matches and ids == {"abcd:1234"}
    return ok, f"where={w} app_matches={matches} usb={sorted(ids)}"


@pytest.fixture
def recover(board):
    """Teardown: base musi byt zdrava (aplikace z buildu, abcd:1234). Zustala-li v bootloaderu po ocekavanem
    preruseni, nahraje se dobry obraz (produktova cesta). Cokoli jineho (visi, cizi obsah, nic na USB): SWD obnova a
    test selze nahlas; nejde-li ani to, pytest.exit (BLOCKED), aby dalsi testy neselhavaly na nasledcich."""
    yield
    board.stop()
    time.sleep(1.5)
    if usb_ids() == {"abcd:1235"}:
        try:
            if board.where()["kind"] == "bootloader":
                board.stop()
                res = flash_good()
                assert res == "ok", f"recovery update failed: {res}"
        finally:
            board.stop()
    ok, why = base_healthy(board)
    if ok:
        return
    board.swd_restore()
    time.sleep(2.0)
    ok2, why2 = base_healthy(board)
    if not ok2:
        pytest.exit(f"BLOCKED: base not recoverable by SWD: {why2}", returncode=3)
    pytest.fail(f"base left in a bad state (repaired by SWD): {why}")


def after_power_loss_expect_bootloader(board):
    """Po 'vypadku' musi base skoncit v bootloaderu (abcd:1235), ne v aplikaci a ne viset."""
    time.sleep(1.0)
    w = board.where()
    board.stop()
    assert w["kind"] == "bootloader", f"after reset: {w}"
    wait_ids("abcd:1235", 8)


# --------------------------------------------------------------------------------------------- Task 2 (hrube)

@pytest.mark.parametrize("pct", [0, 1, 50, 99, 100])
def test_base_abort_at(board, recover, pct):
    """Host prestane posilat po pct % (100 = vsechna data, bez END), pak reset: bootloader zustane, PC updater obnovi."""
    L = enter_bootloader()
    try:
        res = fwclient.Client(L, proto.ADDR_BASE).send_image(IMG, stop_after=fwclient.offset_at(len(IMG), pct))
    finally:
        L.close()
    assert res == "stopped", res
    board.reset()
    after_power_loss_expect_bootloader(board)
    if pct == 50 and UPDATER:
        pc_updater_recover()  # produktova cesta: PC updater nad base, ktera uz je v bootloaderu


@pytest.mark.parametrize("pct", [0, 50, 99])
def test_base_reset_at(board, recover, pct):
    """Reset sondou behem prenosu (chunky v behu): bootloader zustane."""
    L = enter_bootloader()
    target = fwclient.offset_at(len(IMG), pct)
    fired = []

    def on_chunk(off, size):
        if off >= target and not fired:
            fired.append(off)
            board.reset()

    try:
        try:
            res = fwclient.Client(L, proto.ADDR_BASE, retries=0).send_image(IMG, on_chunk=on_chunk)
        except Exception as e:  # noqa: BLE001 - port zmizel pri resetu
            res = f"exception {e!r}"
    finally:
        L.close()
    assert fired, "reset never fired"
    assert res != "ok", res
    after_power_loss_expect_bootloader(board)


BAD = {
    "wrong_board": dict(board=0x03),
    "corrupt_body": dict(corrupt_body=True),
    "corrupt_trailer": dict(corrupt_trailer=True),
    "truncated": dict(truncate=24),
}


@pytest.mark.parametrize("kind", list(BAD))
def test_base_bad_images(board, recover, kind):
    """Vadny obraz se odmitne na END (CRC_FAIL/BAD_SIZE), base zustane v bootloaderu, pak dobry update projde."""
    img = fwclient.make_image(IMG, **BAD[kind])
    L = enter_bootloader()
    try:
        res = fwclient.Client(L, proto.ADDR_BASE).send_image(img)
        print(f"{kind}: {res}")
        assert res in ("END NAK status 3", "END NAK status 5"), res  # FwReceiver: NAK CRC_ERROR + STATUS
        board.reset()
        after_power_loss_expect_bootloader(board)
    finally:
        L.close()
    assert flash_good() == "ok"


def test_base_oversize_rejected_at_start(board, recover):
    L = enter_bootloader()
    try:
        acked, st = fwclient.Client(L, proto.ADDR_BASE, retries=0).start(APP_MAX + 8, 0x12345678)
    finally:
        L.close()
    assert not acked and st == fwclient.ST_BAD_SIZE, (acked, st)


def test_base_wrong_target_keeps_app(board, recover):
    """START s target 0x03 v bootloaderu base: NAK pred mazanim, po resetu nabehne puvodni aplikace."""
    L = enter_bootloader()
    try:
        acked, st = fwclient.Client(L, proto.ADDR_BASE, retries=0).start(len(IMG), zlib.crc32(IMG), target_byte=proto.ADDR_REMOTE)
    finally:
        L.close()
    assert not acked, st
    board.reset()
    time.sleep(1.0)
    w = board.where()
    board.stop()
    assert w["kind"] == "app", w
    wait_ids("abcd:1234", 10)
    assert board.app_matches()


IWDG = 0x40003000  # KR, PR +4, RLR +8, SR +0xC (G431)
DBGMCU_APB1FZR1 = 0xE0042008  # bit 12 DBG_IWDG_STOP


def start_iwdg_2s(board):
    """IWDG ~2 s (LSI 32 kHz /32, RLR 2000) spusteny sondou. IWDG aplikace reset do bootloaderu na G431 neprezije
    (src/wdg.cpp), takze "bootloader s bezicim IWDG" (T7) se jinak na lavici nevyskytne: tady se vynuti."""
    board.tcl(f"write_memory 0x{IWDG:08x} 32 {{0xCCCC}}")
    board.tcl(f"write_memory 0x{IWDG:08x} 32 {{0x5555}}")
    board.tcl(f"write_memory 0x{IWDG + 4:08x} 32 {{3}}")
    board.tcl(f"write_memory 0x{IWDG + 8:08x} 32 {{2000}}")
    wait_until(lambda: board.u32(IWDG + 0xC) == 0, 2, interval=0.05, what="IWDG SR settled")
    board.tcl(f"write_memory 0x{IWDG:08x} 32 {{0xAAAA}}")


def test_base_bootloader_kicks_iwdg(board, recover):
    """T7: bootloader s bezicim IWDG (2 s) 30 s v necinnosti bez resetu, pak cely update (START maze ~1 s) bez resetu.
    Kontrola, ze IWDG opravdu bezel: na konci DBG_IWDG_STOP pryc a jadro 3 s zastavit -> musi prijit reset."""
    L = enter_bootloader()
    L.close()
    start_iwdg_2s(board)
    board.stop()
    n0 = usb_devnum("1235")
    time.sleep(30)
    n1 = usb_devnum("1235")
    assert n0 is not None and n1 == n0, f"bootloader reset while idle with IWDG running: devnum {n0} -> {n1}"
    # update s bezicim IWDG; aplikace po nem IWDG spusti sama (wdg::start)
    assert flash_good() == "ok"
    # negativni kontrola na nove bootloaderu: IWDG spustit, DBG_IWDG_STOP shodit, jadro 3 s zastavit -> reset
    L = enter_bootloader()
    L.close()
    start_iwdg_2s(board)
    fz = board.u32(DBGMCU_APB1FZR1)
    board.tcl(f"write_memory 0x{DBGMCU_APB1FZR1:08x} 32 {{0x{fz & ~(1 << 12):08x}}}")
    m0 = usb_devnum("1235")
    board.halt()
    time.sleep(3)
    board.t.resume()
    board.stop()
    time.sleep(2)
    m1 = usb_devnum("1235")
    assert m1 != m0, f"control failed: no IWDG reset with the core halted 3 s ({m0} -> {m1}) - IWDG not running?"


def test_base_update_x10(board, recover):
    for i in range(10):
        t = time.monotonic()
        res = flash_good(enter_bootloader())
        assert res == "ok", f"update {i}: {res}"
        print(f"update {i}: {time.monotonic() - t:.1f} s")
    assert board.app_matches()


# --------------------------------------------------------------------------------- Task 3 (presne kroky, bp/wp)

class Bg:
    """Klient bezi ve vlakne (jadro stoji na bp, ACK nechodi); vysledek nebo vyjimka v .res."""

    def __init__(self, fn):
        self.res = None
        self.th = threading.Thread(target=self._run, args=(fn,), daemon=True)
        self.th.start()

    def _run(self, fn):
        try:
            self.res = fn()
        except Exception as e:  # noqa: BLE001 - port zmizi pri resetu
            self.res = f"exception {e!r}"

    def join(self, t=30):
        self.th.join(t)
        return self.res


def expect_after_reset(board, kind):
    time.sleep(1.0)
    w = board.where()
    board.stop()
    assert w["kind"] == kind, f"after reset: {w}"
    wait_ids("abcd:1235" if kind == "bootloader" else "abcd:1234", 10)


# Bootloader base je LTO: erasePageAt je inline, zustal jen flashprog::waitBusy (2x na stranku: pred STRT a po nem).
# Pri STARTu se jen maze, takze vstup do mazani stranky n (od 1) = (2n-1). zasah waitBusy.
# n = 1: nic jeste nesmazano -> stara aplikace nabehne; 2: vektorova stranka smazana; 24: uprostred; 47: posledni
# stranka oblasti (0x0801F000).
# Reset sondou (hla, reset_config none) dopadne az par us po rozbehu jadra: pri hit 1 se tak mazani page0 casto jeste
# spusti (bench: 2/2 bootloader). Obojí je bezpecne: hit 1 = aplikace nebo bootloader.
@pytest.mark.parametrize("hit,expect", [(1, ("app", "bootloader")), (2, ("bootloader",)), (24, ("bootloader",)), (47, ("bootloader",))])
def test_base_bp_erase(board, recover, hit, expect):
    L = enter_bootloader()
    try:
        board.arm(board.bl_sym("boot::flashprog::waitBusy"))
        bg = Bg(lambda: fwclient.Client(L, proto.ADDR_BASE, retries=0).send_image(IMG))
        pnb = []
        # zasah 2n-1 = pred STRT stranky n: v CR je jeste PNB predchozi stranky (16 + n - 2); n = 1: zadna
        check = lambda: pnb.append((board.u32(0x40022014) >> 3) & 0x7F)  # noqa: E731
        assert board.break_then_reset(0, hits=2 * hit - 1, armed=True, timeout=20, before_reset=check), "bp not hit"
        if hit > 1:
            assert pnb == [16 + hit - 2], f"bp hit {2 * hit - 1} is not before the erase of app page {hit}: PNB {pnb}"
        print("client:", bg.join())
    finally:
        L.close()
    time.sleep(1.0)
    w = board.where()
    print(f"hit {hit}: {w}")
    board.stop()
    assert w["kind"] in expect, f"after reset: {w}"
    wait_ids("abcd:1235" if w["kind"] == "bootloader" else "abcd:1234", 10)
    if w["kind"] == "app":
        assert board.app_matches()


def _send_all_but_end(L):
    res = fwclient.Client(L, proto.ADDR_BASE).send_image(IMG, stop_after=len(IMG))
    assert res == "stopped", res


# Vypadek pri zapisu vektorove stranky (page0 z RAM, programuje se az ve finish()): wp na dvojslove k zastavi jadro
# az po nekolika dalsich instrukcich (DWT na M4 je nepresny; k = 255 uz stihne cele page0) a reset pak trefi
# programovani nektereho dvojslova. Pozadavek: bootloader nebo nova aplikace, nikdy viset (bench 2026-10-24: k = 0
# -> ECC dvojita chyba v page0 -> NMI v bootloaderu -> Default_Handler, i po kazdem dalsim resetu; nalez F4).
F4 = "F4: interrupted page0 programming -> ECC double error -> bootloader NMI -> Default_Handler forever (no USB)"
F5 = "F5: trailer page lost -> NoTrailer accepted (BOOT_REQUIRE_TRAILER=0) -> truncated app boots and faults"


@pytest.mark.parametrize("k", [pytest.param(0, marks=pytest.mark.xfail(strict=True, raises=bootfi.ProductDefect, reason=F4)),
                               1, 2, 128, 255])
def test_base_bp_commit_page0(board, recover, k):
    L = enter_bootloader()
    try:
        _send_all_but_end(L)
        board.arm(APP + 8 * k, watch_write=True)
        bg = Bg(lambda: fwclient.Client(L, proto.ADDR_BASE, retries=0).end())
        assert board.break_then_reset(0, armed=True, timeout=20), "wp not hit"
        print("client:", bg.join())
    finally:
        L.close()
    time.sleep(1.0)
    w = board.where()
    print(f"k={k}: after reset {w}, ECCR {board.eccr():#x}, app[0:16] {board.flash_read(APP, 16).hex()}")
    if w["kind"] == "hang":
        eccr = board.eccr()
        board.swd_restore()  # sama obnovit: recover pak hlida jen chyby harnessu
        board.stop()
        raise bootfi.ProductDefect(f"after a reset during the page0 commit: {w}, ECCR {eccr:#x}")
    board.stop()
    assert w["kind"] in ("bootloader", "app"), f"after a reset during the page0 commit: {w}"


def test_base_bp_after_commit(board, recover):
    """Vypadek hned po zapisu vektorove stranky (wp na druhem slove posledniho dvojslova page0: jadro stoji po zapisu,
    programovani dvojslova 85 us dobehne), pred zaverecnou kontrolou CRC a ACK: nova aplikace nabehne."""
    L = enter_bootloader()
    try:
        _send_all_but_end(L)
        board.arm(APP + 0x7FC, watch_write=True)
        bg = Bg(lambda: fwclient.Client(L, proto.ADDR_BASE, retries=0).end())
        assert board.break_then_reset(0, armed=True, timeout=20), "bp not hit"
        print("client:", bg.join())
    finally:
        L.close()
    expect_after_reset(board, "app")
    assert board.app_matches()


# ------------------------------------------------------------------------ Task 4 (preruseni flash operace, T1)

OUT = os.environ.get("BASETEST_OUT", "/tmp")
SCRATCH = 0x0801F000  # posledni stranka oblasti aplikace pred nastavenim; obraz ji nepouziva (prazdna)
# Podil zmerene doby operace (glitch delay a mereni maji od 2026-10-25 stejne telo smycky). Huste v cele dobe operace,
# aby kratke okno ECC chyby (u dvojslova ~25-50 %) trefil kazdy beh.
FRACTIONS = [0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 1.0, 1.1]


def glitch_log(name, rows):
    import json
    with open(os.path.join(OUT, f"glitch-{name}.json"), "w") as f:
        json.dump(rows, f, indent=1)


def settle_and_probe(board):
    """Po glitch resetu: kde jadro je, ECCR, prvnich 8 B oblasti aplikace."""
    time.sleep(1.5)
    w = board.where()
    return dict(where=w["kind"], pc=hex(w["pc"]), ipsr=w["ipsr"], eccr=hex(board.eccr()), usb=sorted(usb_ids()),
                head=board.flash_read(APP, 8).hex())


def measure(board, op, addr, **kw):
    """Doba operace v iteracich glitch smycky, merena ve stejnem stavu (aplikace 144 MHz / bootloader HSI16), ve kterem
    se pak glitchuje. Chybove priznaky z predchoziho stavu (bench: jednou SR 0xa8) -> jeden opakovany pokus."""
    n = board.glitch(op, addr, **kw)
    if n >= 0x80000000:
        print(f"measure: flash error {n:#x}, retrying")
        time.sleep(1.0)
        n = board.glitch(op, addr, **kw)
    assert n < 0x80000000, f"flash error during measure: {n:#x}"
    return n


def _sweep(board, name, prepare, op, addr, n_full, w=(0, 0)):
    rows = []
    for frac in FRACTIONS:
        prepare()
        delay = int(n_full * frac)
        board.glitch(op, addr, delay=delay, w0=w[0], w1=w[1])
        r = settle_and_probe(board)
        r.update(frac=frac, delay=delay)
        print(f"{name} frac {frac:4.2f} delay {delay:7d}: {r}")
        rows.append(r)
    glitch_log(name, rows)
    return rows


def test_base_glitch_erase_vector_page(board, recover):
    """Reset v prubehu mazani vektorove stranky platne aplikace (0..120 % doby mazani). Nikdy nesmi viset."""
    n_erase = measure(board, bootfi.OP_MEASURE_ERASE, SCRATCH)
    print(f"erase takes {n_erase} iterations")

    def prepare():
        if not board.app_matches():
            board.swd_restore()
            time.sleep(1.0)

    rows = _sweep(board, "base-erase-vector", prepare, bootfi.OP_ERASE, APP, n_erase)
    board.swd_restore()
    hangs = [r for r in rows if r["where"] == "hang"]
    assert not hangs, f"base hung after an interrupted erase: {hangs}"


@pytest.mark.xfail(strict=True, raises=bootfi.ProductDefect, reason=F4)
def test_base_glitch_program_vector(board, recover):
    """Vektorova stranka smazana a hned reset v prubehu programovani 1. dvojslova (SP, reset vektor), vse z jedne glitch
    rutiny ve stavu aplikace (0..120 % doby programovani). Nikdy nesmi viset."""
    w0, w1 = int.from_bytes(IMG[:4], "little"), int.from_bytes(IMG[4:8], "little")
    n_prog = measure(board, bootfi.OP_MEASURE_ERASE_THEN_PROGRAM, SCRATCH, w0=w0, w1=w1)
    measure(board, bootfi.OP_MEASURE_ERASE, SCRATCH)
    print(f"program takes {n_prog} iterations")

    def prepare():
        if not board.app_matches() or board.where()["kind"] != "app":
            board.swd_restore()

    rows = _sweep(board, "base-program-vector", prepare, bootfi.OP_ERASE_THEN_PROGRAM, APP, n_prog, w=(w0, w1))
    board.swd_restore()
    hangs = [r for r in rows if r["where"] == "hang"]
    if hangs:
        raise bootfi.ProductDefect(f"base hung after an interrupted program: {hangs}")


@pytest.mark.xfail(strict=True, raises=bootfi.ProductDefect, reason=F5)
def test_base_glitch_erase_trailer_page(board, recover):
    """Reset v prubehu mazani posledni pouzite stranky (trailer) platne aplikace: bootloader nebo aplikace, nikdy viset."""
    last = APP + (len(IMG) - 1) // 2048 * 2048
    n_erase = measure(board, bootfi.OP_MEASURE_ERASE, SCRATCH)

    def prepare():
        if not board.app_matches():
            board.swd_restore()
            time.sleep(1.0)

    rows = _sweep(board, "base-erase-trailer", prepare, bootfi.OP_ERASE, last, n_erase)
    board.swd_restore()
    hangs = [r for r in rows if r["where"] == "hang"]
    if hangs:
        raise bootfi.ProductDefect(f"base hung after an interrupted trailer-page erase: {hangs}")
