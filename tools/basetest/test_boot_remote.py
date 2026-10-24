"""Odolnost OTA remote a jeho bootloaderu (plan docs/superpowers/plans/2026-10-24-bootloader-robustness-tests.md,
Tasks 5-6). OTA jde fwclientem pres base (protokolovy CDC, keepalive). Dva rozlisitelne obrazy se stejnym kodem:
A = build/midicontroller-fw-update.bin, B = totez s jinou verzi v traileru (jiny trailer -> jiny obsah flash), takze
je z flash videt, ktery obraz remote ma. Na zacatku a konci kazdeho testu ma remote A nebo B, je spojen s base a
jadro nevisi; jinak fixture obnovi A pres SWD a test selze.

Bp v bootloaderu remote: FPB/DWT systemovy reset neprezije jen POR, proto se bp nastavi v bezici aplikaci a zachyti
se po jejim resetu (OTA END -> NVIC_SystemReset -> bootloader aplikuje)."""

import os
import time

import pytest

import bootfi
import fwclient
import proto
import usblink
from conftest import remote_alive, wait_until

R = bootfi.BOARDS["remote"]
APP = R["app"]
IMG_A = open(R["update_bin"], "rb").read()
IMG_B = fwclient.make_image(IMG_A, version=0x00FF01)
N_PAGES = (R["app_end"] - APP) // R["page"]  # 55
OUT = os.environ.get("BASETEST_OUT", "/tmp")


def installed(rb, settle=20.0) -> str:
    """A | B | other. Behem aplikace bootloaderem (kopie ~ sekundy) se obsah meni: dokud neni A ani B a jadro neni v
    aplikaci, pockat (max `settle` s)."""
    end = time.monotonic() + settle
    while True:
        for name, img in (("A", IMG_A), ("B", IMG_B)):
            if rb.app_matches(img):
                return name
        if time.monotonic() > end or rb.where()["kind"] == "app":
            return "other"
        time.sleep(1.0)


def other(name):
    return IMG_B if name == "A" else IMG_A


@pytest.fixture
def base():
    b = bootfi.Board("base")
    yield b
    b.stop()


@pytest.fixture
def rb():
    b = bootfi.Board("remote")
    yield b
    b.stop()


def linked(base_board, timeout=40):
    wait_until(lambda: base_board.t.read_bool("Bluetooth::_streamOpen") and remote_alive(base_board.t), timeout, interval=0.5,
               what="remote link up")


@pytest.fixture
def link(base):
    linked(base)
    L = usblink.Link(keepalive=True)
    yield L
    L.close()


@pytest.fixture
def remote_ok(rb, base):
    """Pred testem: A nebo B, spojeni. Po testu: A nebo B nainstalovane, jadro v aplikaci, spojeni zpet."""
    linked(base)
    yield
    hung = None
    w = rb.where()
    if w["kind"] != "app":
        time.sleep(15)  # mozna jeste aplikuje (kopie trva sekundy)
        w = rb.where()
    if w["kind"] != "app" or installed(rb) == "other":
        hung = dict(where=w, installed=installed(rb))
        rb.disarm_all()
        rb.swd_restore()
    rb.stop()
    linked(base, 60)
    base.stop()
    assert hung is None, f"remote not back in a known app (recovered by SWD): {hung}"


def ota(link, img, **kw) -> str:
    return fwclient.Client(link, proto.ADDR_REMOTE).send_image(img, **kw)


def wait_app(rb, timeout=30):
    """Po resetu remote: ceka, az jadro bezi v aplikaci (bootloader muze kopirovat ~ sekundy)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        w = rb.where()
        if w["kind"] == "app":
            return w
        time.sleep(1.0)
    return rb.where()


def no_reapply(rb, timeout=4.0) -> bool:
    """Reset remote s bp na flashprog::eraseRange: nezasazeny = pending je smazany (zadna dalsi kopie)."""
    rb.arm(rb.bl_sym("boot::flashprog::eraseRange"))
    rb.reset()
    hit = rb.wait_halt(timeout)
    rb.disarm_all()
    if rb.halted():
        rb.t.resume()
    return not hit


# ------------------------------------------------------------------------------- Task 5: preruseny staging

@pytest.mark.parametrize("how", ["stop_50", "no_end", "ble_reset_50", "base_reset_50", "remote_reset_50"])
def test_remote_staging_abort(rb, base, link, remote_ok, how):
    """Preruseny prenos: remote dal bezi se starym obrazem, reset ho neaplikuje; pak cely OTA projde."""
    before = installed(rb)
    assert before in ("A", "B")
    img = other(before)
    half = fwclient.offset_at(len(img), 50)
    rb.stop()
    if how == "stop_50":
        res = ota(link, img, stop_after=half)
    elif how == "no_end":
        res = ota(link, img, stop_after=len(img))
    else:
        fired = []

        def on_chunk(off, size):
            if off >= half and not fired:
                fired.append(off)
                if how == "ble_reset_50":
                    rb.t.hold_ble_reset(True)
                    time.sleep(3)
                    rb.t.hold_ble_reset(False)
                elif how == "base_reset_50":
                    base.reset()
                else:
                    rb.reset()

        try:
            res = fwclient.Client(link, proto.ADDR_REMOTE, retries=1).send_image(img, on_chunk=on_chunk)
        except Exception as e:  # noqa: BLE001 - base reset -> CDC zmizi
            res = f"exception {e!r}"
        assert fired
    print(f"{how}: {res}")
    if res == "ok":
        # prenos vydrzel (napr. 3 s RN4870 v resetu: retry PC + znovupripojeni): pak musi byt novy obraz nainstalovany
        assert how in ("ble_reset_50",), f"{how}: transfer reported ok"
        time.sleep(2)
        assert wait_app(rb, 40)["kind"] == "app" and installed(rb) == ("B" if before == "A" else "A")
        rb.stop()
        linked(base, 60)
        base.stop()
        return
    if how == "base_reset_50":
        link.detach()
        link.attach(settle=2.0)
        link.error = None  # vlakno spadlo pri zmizeni CDC (ocekavane); attach ho nahradil
    rb.stop()
    base.stop()
    # reset remote: bootloader nesmi nic aplikovat
    assert no_reapply(rb), "bootloader applied an interrupted staging"
    assert wait_app(rb)["kind"] == "app"
    assert installed(rb) == before
    rb.stop()
    linked(base, 60)
    base.stop()
    res = ota(link, img)
    assert res == "ok", res
    rb.stop()
    time.sleep(2)
    assert wait_app(rb)["kind"] == "app"
    assert installed(rb) == ("B" if before == "A" else "A")


BAD = {
    "wrong_board": dict(board=0x02),
    "corrupt_body": dict(corrupt_body=True),
    "corrupt_trailer": dict(corrupt_trailer=True),
    "truncated": dict(truncate=24),
}


@pytest.mark.parametrize("kind", list(BAD))
def test_remote_bad_images(rb, base, link, remote_ok, kind):
    """Vadny obraz: END NAK (CRC_FAIL/BAD_SIZE), remote se nerestartuje, pending se nenastavi."""
    before = installed(rb)
    rb.stop()
    res = ota(link, fwclient.make_image(other(before), **BAD[kind]))
    print(f"{kind}: {res}")
    assert res in ("END NAK status 3", "END NAK status 5"), res
    assert rb.where()["kind"] == "app"
    assert no_reapply(rb), "bad image was staged as pending"
    assert wait_app(rb)["kind"] == "app" and installed(rb) == before


# ------------------------------------------------------------------- Task 5: vypadek pri aplikaci v bootloaderu

def stage_and_break(rb, link, addr, *, hits=1, watch_write=False, timeout=60, before_reset=None):
    """OTA druheho obrazu s bp nastavenym pred END (zasahne az bootloader po resetu aplikace); na `hits`-tem zasahu
    reset. Vraci (pred, cil)."""
    before = installed(rb)
    img = other(before)
    rb.stop()
    res = ota(link, img, stop_after=len(img))
    assert res == "stopped", res
    rb.arm(addr, watch_write)
    res = fwclient.Client(link, proto.ADDR_REMOTE).end()
    assert res[0] and res[1] == fwclient.ST_CRC_OK, res
    assert rb.break_then_reset(0, hits=hits, armed=True, timeout=timeout, before_reset=before_reset), "bp not hit"
    return before, ("B" if before == "A" else "A")


def converged(rb, want):
    w = wait_app(rb, 40)
    assert w["kind"] == "app", w
    assert installed(rb) == want, f"installed {installed(rb)}, want {want}"
    assert no_reapply(rb), "pending still set after a completed apply"
    assert wait_app(rb)["kind"] == "app"


@pytest.mark.parametrize("hit", [1, 2, N_PAGES // 2, N_PAGES])
def test_remote_bp_erase(rb, base, link, remote_ok, hit):
    _, want = stage_and_break(rb, link, rb.bl_sym("boot::flashprog::erasePageAt"), hits=hit)
    converged(rb, want)


def test_remote_bp_copy_mid(rb, base, link, remote_ok):
    mid = APP + (len(IMG_A) // 2) // 8 * 8
    _, want = stage_and_break(rb, link, mid, watch_write=True)
    converged(rb, want)


@pytest.mark.parametrize("k", [0, 1, 255])
def test_remote_bp_vector_copy(rb, base, link, remote_ok, k):
    """Vektorova stranka se kopiruje posledni; reset po k dvojslovech (k = 1: SP + reset vektor uz zapsane)."""
    _, want = stage_and_break(rb, link, APP + 8 * k, watch_write=True)
    converged(rb, want)


def test_remote_bp_before_clear(rb, base, link, remote_ok):
    _, want = stage_and_break(rb, link, rb.bl_sym("boot::clearStagePending"))
    converged(rb, want)


def test_remote_bp_clear_between(rb, base, link, remote_ok):
    """Reset mezi smazanim a zapisem hlavicky v clearStagePending (writeData vola jen ono): bez magic -> zadna kopie."""
    _, want = stage_and_break(rb, link, rb.bl_sym("boot::w25q::writeData"))
    converged(rb, want)


def test_remote_reset_every_boot_converges(rb, base, link, remote_ok):
    """Reset uprostred kopie na 3 bootech po sobe; 4. boot dokonci."""
    copy = rb.bl_sym("boot::copyRange")
    _, want = stage_and_break(rb, link, copy, hits=1)
    for i in range(2):
        assert rb.break_then_reset(copy, hits=1, timeout=20), f"boot {i + 2}: copy not retried"
    converged(rb, want)


# --------------------------------------------------------------- Task 5: chyby SPI / flash pri aplikaci

def _force_return_false(rb):
    rb.tcl("reg r0 0")
    rb.tcl(f"reg pc 0x{rb.reg('lr') & ~1:08x}")


def _run_with_forced_failures(rb, should_fail, timeout=6.0):
    """Bp (uz nastaveny pred END) na funkci: pri kazdem zasahu, kde should_fail(lr), funkce hned vrati false. Konci po
    `timeout` bez zasahu. Vraci pocet vynucenych chyb."""
    forced = 0
    try:
        while rb.wait_halt(timeout):
            lr = rb.reg("lr")
            if should_fail(lr):
                _force_return_false(rb)
                forced += 1
            rb.t.resume()
    finally:
        rb.disarm_all()
        if rb.halted():
            rb.t.resume()
    return forced


def _in_apply(rb):
    a = rb.bl_sym("boot::applyStagedImageIfPending")
    return lambda lr: a <= lr < a + 0x260


def test_remote_spi_read_error_every_attempt(rb, base, link, remote_ok):
    """SPI cteni pri overeni obrazu selze ve vsech 3 pokusech: stara aplikace nabehne netknuta, pending zustane, dalsi
    reset obraz aplikuje."""
    before = installed(rb)
    img = other(before)
    rb.stop()
    assert ota(link, img, stop_after=len(img)) == "stopped"
    rb.arm(rb.bl_sym("boot::w25q::read"))
    res = fwclient.Client(link, proto.ADDR_REMOTE).end()
    assert res[0], res
    forced = _run_with_forced_failures(rb, _in_apply(rb))
    print(f"forced SPI read failures: {forced}")
    assert forced == 3, forced
    assert wait_app(rb)["kind"] == "app" and installed(rb) == before
    rb.reset()  # pending zustal -> aplikuje
    converged(rb, "B" if before == "A" else "A")


def test_remote_bad_staged_image(rb, base, link, remote_ok):
    """Bajt obrazu se pri overeni v bootloaderu precte spatne (preklopeny v s_buf pred CRC): BadImage -> pending smazan,
    stara aplikace netknuta."""
    before = installed(rb)
    img = other(before)
    rb.stop()
    assert ota(link, img, stop_after=len(img)) == "stopped"
    upd = rb.bl_sym("boot::Crc32::update")
    rb.arm(upd)
    res = fwclient.Client(link, proto.ADDR_REMOTE).end()
    assert res[0], res
    flipped = False
    try:
        while not flipped and rb.wait_halt(20):
            if rb.reg("r2") == 256:  # chunk s_buf ve stagedImageGood (trailer CRC ma 16 B)
                p = rb.reg("r1")
                b = rb.flash_read(p, 4)
                rb.tcl(f"write_memory 0x{p:08x} 8 {{0x{b[0] ^ 0xFF:02x}}}")
                flipped = True
            rb.t.resume()
    finally:
        rb.disarm_all()
        if rb.halted():
            rb.t.resume()
    assert flipped
    assert wait_app(rb)["kind"] == "app" and installed(rb) == before
    assert no_reapply(rb), "pending not cleared after a bad staged image"
    assert wait_app(rb)["kind"] == "app"


def test_remote_ota_x3(rb, base, link, remote_ok):
    for i in range(3):
        before = installed(rb)
        rb.stop()
        t = time.monotonic()
        assert ota(link, other(before)) == "ok"
        time.sleep(2)
        assert wait_app(rb)["kind"] == "app"
        print(f"OTA {i}: {time.monotonic() - t:.1f} s -> {installed(rb)}")
        assert installed(rb) == ("B" if before == "A" else "A")
        rb.stop()
        linked(base, 60)
        base.stop()


@pytest.mark.parametrize("dummy", [0])
def test_remote_program_error_3x(rb, base, link, remote_ok, dummy):
    """flashprog::program selze ve vsech 3 pokusech (po smazani oblasti): zadna bootovatelna aplikace -> bootloader
    pusti PA1 a ceka; reset bez chyby obraz aplikuje. Pokud se remote pri uvolneni PA1 vypne, test to nahlasi."""
    before = installed(rb)
    img = other(before)
    rb.stop()
    assert ota(link, img, stop_after=len(img)) == "stopped"
    prog = rb.bl_sym("boot::flashprog::program")
    rb.arm(prog)
    res = fwclient.Client(link, proto.ADDR_REMOTE).end()
    assert res[0], res
    forced = _run_with_forced_failures(rb, lambda lr: True)
    print(f"forced program failures: {forced}")
    assert forced == 3, forced
    time.sleep(1)
    w = rb.where()
    odr = rb.u32(0x50000014)
    print(f"after 3 failed copies: {w}, GPIOA ODR {odr:#x}")
    assert w["kind"] == "bootloader", w
    assert not (odr & 0x2), "power hold PA1 still high with no bootable app"
    rb.reset()
    converged(rb, "B" if before == "A" else "A")


# ------------------------------------------------------------------------- Task 6: preruseni flash operace (T1)

SCRATCH = 0x0801F000
FRACTIONS = [0.0, 0.02, 0.1, 0.25, 0.5, 0.75, 0.9, 0.98, 1.2]


def _probe(rb):
    time.sleep(2.0)
    try:
        w = rb.where()
        return dict(where=w["kind"], pc=hex(w["pc"]), ipsr=w["ipsr"], eccr=hex(rb.eccr()), head=rb.flash_read(APP, 8).hex(),
                    pa1=bool(rb.u32(0x50000014) & 2))
    except Exception as e:  # noqa: BLE001 - remote se mohl vypnout (PA1)
        rb.stop()
        return dict(where="off?", error=repr(e)[:120])


def _sweep(rb, name, prepare, op, addr, n_full, w=(0, 0)):
    import json
    rows = []
    for frac in FRACTIONS:
        prepare()
        delay = int(n_full * frac)
        rb.glitch(op, addr, delay=delay, w0=w[0], w1=w[1])
        r = _probe(rb)
        r.update(frac=frac, delay=delay)
        print(f"{name} frac {frac:4.2f} delay {delay:7d}: {r}")
        rows.append(r)
    with open(os.path.join(OUT, f"glitch-{name}.json"), "w") as f:
        json.dump(rows, f, indent=1)
    return rows


def _restore_if_needed(rb):
    if installed(rb) == "other" or rb.where()["kind"] != "app":
        rb.swd_restore()
        time.sleep(1.0)


def test_remote_glitch_erase_vector_page(rb, base, remote_ok):
    n = rb.glitch(bootfi.OP_MEASURE_ERASE, SCRATCH)
    print(f"erase takes {n} iterations")
    rows = _sweep(rb, "remote-erase-vector", lambda: _restore_if_needed(rb), bootfi.OP_ERASE, APP, n)
    _restore_if_needed(rb)
    bad = [r for r in rows if r["where"] in ("hang", "off?", "unknown")]
    assert not bad, f"remote hung/off after an interrupted erase: {bad}"


def test_remote_glitch_program_vector(rb, base, remote_ok):
    img = IMG_A if installed(rb) == "A" else IMG_B
    w0, w1 = int.from_bytes(img[:4], "little"), int.from_bytes(img[4:8], "little")
    n_prog = rb.glitch(bootfi.OP_MEASURE_PROGRAM, SCRATCH, w0=w0, w1=w1)
    rb.glitch(bootfi.OP_MEASURE_ERASE, SCRATCH)
    rows = _sweep(rb, "remote-program-vector", lambda: rb.glitch(bootfi.OP_MEASURE_ERASE, APP), bootfi.OP_PROGRAM, APP,
                  n_prog, w=(w0, w1))
    rb.swd_restore()
    bad = [r for r in rows if r["where"] in ("hang", "off?", "unknown")]
    assert not bad, f"remote hung/off after an interrupted program: {bad}"
