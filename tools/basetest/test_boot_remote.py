"""Robustness of the remote OTA and its bootloader (plan docs/superpowers/plans/2026-10-24-bootloader-robustness-tests.md,
Tasks 5-6). OTA goes via fwclient through the base (protocol CDC, keepalive). Two distinguishable images with the same code:
A = build/midicontroller-fw-update.bin, B = the same with a different version in the trailer (different trailer -> different flash content), so
the flash shows which image the remote has. At the start and end of every test the remote has A or B, is linked to the base and the
core is not hung; otherwise the fixture restores A over SWD and the test fails.

Bp in the remote bootloader: FPB/DWT survive a system reset, only POR clears them, so the bp is set in the running application and caught
after its reset (OTA END -> NVIC_SystemReset -> the bootloader applies)."""

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
    """A | B | other. While the bootloader is applying (copy ~ seconds) the content changes: as long as it is neither A nor B and the core is not in
    the application, wait (max `settle` s)."""
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


F6_LOG = os.path.join(OUT, "f6-silent-remote.log")


def _remote_sym(suffix: str) -> int:
    import subprocess
    elf = os.path.join(bootfi.REMOTE_FW, "build", "midicontroller-fw.elf")
    out = subprocess.run(["arm-none-eabi-nm", "-C", elf], capture_output=True, text=True, check=True).stdout
    return next(int(l.split()[0], 16) for l in out.splitlines() if l.endswith(suffix))


def remote_never_received() -> bool:
    """The remote has received nothing from the base since boot: link::s_lastRx == 0 and link::s_keepaliveSeen == 0 (the base sends keepalive
    every 2 s)."""
    rb = bootfi.Board("remote")
    try:
        last = rb.u32(_remote_sym("link::(anonymous namespace)::s_lastRx"))
        seen = rb.flash_read(_remote_sym("link::(anonymous namespace)::s_keepaliveSeen"), 1)[0]
        return last == 0 and seen == 0
    finally:
        rb.stop()


def linked(base_board, timeout=40):
    """Link to the remote. Finding F6: after a remote reboot the base can report a secured link, but the remote neither receives nor sends anything since
    boot and nothing fixes it (not even a disconnect/connect from the base). It is recognized here (30 s of silence on a secured link), written to F6_LOG and the remote is
    reset so that further tests can run; the number of occurrences is in the report."""
    t = base_board.t
    ok = lambda: t.read_bool("Bluetooth::_streamOpen") and remote_alive(t)  # noqa: E731
    try:
        wait_until(ok, min(timeout, 30), interval=0.5, what="remote link up")
        return
    except AssertionError:
        pass
    if t.read_bool("Bluetooth::_streamOpen") and t.read_bool("Bluetooth::_peerAccepted") and remote_never_received():
        with open(F6_LOG, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S')} F6: base secured link, remote received nothing since boot; base log tail: {t.debug_log()[-6:]}\n")
        print("F6 occurrence: silent remote on a secured link -> remote reset to continue")
        rb = bootfi.Board("remote")
        rb.reset()
        rb.stop()
    base_board.stop()
    wait_until(ok, max(timeout, 60), interval=0.5, what="remote link up")


@pytest.fixture
def link(base):
    linked(base)
    L = usblink.Link(keepalive=True)
    yield L
    L.close()


@pytest.fixture
def remote_ok(rb, base):
    """Before the test: A or B, linked. After the test: A or B installed, core in the application, link back."""
    linked(base)
    yield
    hung = None
    try:
        w = rb.where()
        if w["kind"] != "app":
            time.sleep(15)  # it may still be applying (the copy takes seconds)
            w = rb.where()
        if w["kind"] != "app" or installed(rb) == "other":
            hung = dict(where=w, installed=installed(rb))
            rb.disarm_all()
            rb.swd_restore()
            if rb.where()["kind"] != "app" or installed(rb, settle=2) == "other":
                pytest.exit(f"BLOCKED: remote not recoverable by SWD after {hung}", returncode=3)
    except (RuntimeError, ValueError, OSError) as e:
        pytest.exit(f"BLOCKED (needs_user?): remote not reachable through its probe - switched off? {e!r}", returncode=3)
    finally:
        rb.stop()
    linked(base, 60)
    base.stop()
    assert hung is None, f"remote not back in a known app (recovered by SWD): {hung}"


def ota(link, img, **kw) -> str:
    return fwclient.Client(link, proto.ADDR_REMOTE).send_image(img, **kw)


def wait_app(rb, timeout=30):
    """After a remote reset: waits until the core runs in the application (the bootloader may copy for ~ seconds)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        w = rb.where()
        if w["kind"] == "app":
            return w
        time.sleep(1.0)
    return rb.where()


def no_reapply(rb, timeout=4.0) -> bool:
    """Remote reset with a bp on flashprog::eraseRange: not hit = pending is cleared (no further copy)."""
    rb.arm(rb.bl_sym("boot::flashprog::eraseRange"))
    rb.reset()
    hit = rb.wait_halt(timeout)
    rb.disarm_all()
    if rb.halted():
        rb.t.resume()
    return not hit


# ------------------------------------------------------------------------------- Task 5: interrupted staging

@pytest.mark.parametrize("how", ["stop_50", "no_end", "ble_reset_50", "base_reset_50", "remote_reset_50"])
def test_remote_staging_abort(rb, base, link, remote_ok, how):
    """Interrupted transfer: the remote keeps running the old image, a reset does not apply it; then a full OTA goes through."""
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
        except Exception as e:  # noqa: BLE001 - base reset -> the CDC disappears
            res = f"exception {e!r}"
        assert fired
    print(f"{how}: {res}")
    if res == "ok":
        # the transfer survived (e.g. 3 s of RN4870 in reset: PC retry + reconnection): then the new image must be installed
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
        link.error = None  # the thread died when the CDC disappeared (expected); attach replaced it
    rb.stop()
    base.stop()
    # remote reset: the bootloader must not apply anything
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
    """Bad image: END NAK (CRC_FAIL/BAD_SIZE), the remote does not restart, pending is not set."""
    before = installed(rb)
    rb.stop()
    res = ota(link, fwclient.make_image(other(before), **BAD[kind]))
    print(f"{kind}: {res}")
    assert res in ("END NAK status 3", "END NAK status 5"), res
    assert rb.where()["kind"] == "app"
    assert no_reapply(rb), "bad image was staged as pending"
    assert wait_app(rb)["kind"] == "app" and installed(rb) == before


# ------------------------------------------------------------------- Task 5: power loss during apply in the bootloader

def stage_halted_at_reboot(rb, link):
    """OTA of the other image up to and including END, application halted at fwupdate::reboot() (before its own reset; pending = 1 is
    written). The bootloader bp is then set while the core is halted and openocd does the reset itself ("reset run"): after a reset
    that the remote does by itself, openocd (hla) loses the core and a new openocd clears FPB/DWT on examine (bench 2026-10-24),
    so late bps in the bootloader were missed. Returns (before, target)."""
    before = installed(rb)
    img = other(before)
    rb.stop()
    res = ota(link, img, stop_after=len(img))
    assert res == "stopped", res
    rb.arm(_remote_sym("fwupdate::reboot()"))
    res = fwclient.Client(link, proto.ADDR_REMOTE).end()
    assert res[0] and res[1] == fwclient.ST_CRC_OK, res
    assert rb.wait_halt(30), "app did not reach fwupdate::reboot()"
    rb.tcl("halt")
    rb.disarm_all()
    return before, ("B" if before == "A" else "A")


def boot_bootloader(rb):
    """From the halted application (stage_halted_at_reboot) a probe reset -> the bootloader applies; the bp is already set."""
    rb.tcl("reset run")


def stage_and_break(rb, link, addr, *, hits=1, watch_write=False, timeout=60, before_reset=None, expect_r0=None):
    """A bp in the bootloader at `addr` (the hits-th hit), then reset = a power loss there. Returns (before, target). `expect_r0`: the first
    argument of the function at the hit (page/block address) - a check that the counted hit did not skip (after a connection recovery
    FPB is cleared and set again; review 2026-10-25)."""
    before, want = stage_halted_at_reboot(rb, link)
    rb.arm(addr, watch_write)
    boot_bootloader(rb)
    seen = []

    def at_hit():
        if expect_r0 is not None:
            rb.tcl("halt")
            seen.append(rb.reg("r0"))
        if before_reset:
            before_reset()

    assert rb.break_then_reset(0, hits=hits, armed=True, timeout=timeout, before_reset=at_hit), f"bp 0x{addr:08x} not hit"
    if expect_r0 is not None:
        assert seen == [expect_r0], f"hit {hits} of 0x{addr:08x} had r0 {[hex(x) for x in seen]}, want {expect_r0:#x}"
    return before, want


def _base_board():
    return bootfi.Board("base")


def converged(rb, want):
    w = wait_app(rb, 40)
    assert w["kind"] == "app", w
    assert installed(rb) == want, f"installed {installed(rb)}, want {want}"
    assert no_reapply(rb), "pending still set after a completed apply"
    assert wait_app(rb)["kind"] == "app"


@pytest.mark.parametrize("hit", [1, 2, N_PAGES // 2, N_PAGES])
def test_remote_bp_erase(rb, base, link, remote_ok, hit):
    _, want = stage_and_break(rb, link, rb.bl_sym("boot::flashprog::erasePageAt"), hits=hit,
                              expect_r0=APP + (hit - 1) * R["page"])
    converged(rb, want)


# Copy: copyRange(2048, size) and then copyRange(0, 2048) in 256 B steps = one flashprog::program call per block. A bp on
# program with a hit count (DWT watchpoints on flash addresses were unreliable on the M0+ with hla: 2 of 4 runs missed).
PROG_REST = (len(IMG_A) - 2048 + 255) // 256  # calls before the vector page


def test_remote_bp_copy_mid(rb, base, link, remote_ok):
    h = PROG_REST // 2
    _, want = stage_and_break(rb, link, rb.bl_sym("boot::flashprog::program"), hits=h, expect_r0=APP + 2048 + 256 * (h - 1))
    converged(rb, want)


# j = vector page block (0..7) before whose write the "power loss" comes: 0 = nothing of the vector page, 1 = the first
# block (SP, reset vector, first vectors) already written, 7 = everything except the last block.
@pytest.mark.parametrize("j", [0, 1, 7])
def test_remote_bp_vector_copy(rb, base, link, remote_ok, j):
    _, want = stage_and_break(rb, link, rb.bl_sym("boot::flashprog::program"), hits=PROG_REST + 1 + j, expect_r0=APP + 256 * j)
    converged(rb, want)


def test_remote_bp_before_clear(rb, base, link, remote_ok):
    _, want = stage_and_break(rb, link, rb.bl_sym("boot::clearStagePending"))
    converged(rb, want)


def test_remote_bp_clear_between(rb, base, link, remote_ok):
    """Reset between the erase and the header write in clearStagePending (only it calls writeData): without magic -> no copy.
    A bp on writeData reliably missed after a remote reset (3/3); so halt at clearStagePending (that works), then
    set writeData while the core is halted and run up to it."""
    wd = rb.bl_sym("boot::w25q::writeData")
    reached = []

    def to_write_data():
        rb.disarm_all()
        rb.arm(wd)
        rb.tcl("resume")
        reached.append(rb.wait_halt(10))

    _, want = stage_and_break(rb, link, rb.bl_sym("boot::clearStagePending"), before_reset=to_write_data)
    assert reached == [True], "writeData not reached after clearStagePending"
    converged(rb, want)


def test_remote_reset_every_boot_converges(rb, base, link, remote_ok):
    """Reset in the middle of the copy on 3 boots in a row; the 4th boot completes it."""
    copy = rb.bl_sym("boot::copyRange")
    _, want = stage_and_break(rb, link, copy, hits=1)
    for i in range(2):
        assert rb.break_then_reset(copy, hits=1, timeout=20), f"boot {i + 2}: copy not retried"
    converged(rb, want)


# --------------------------------------------------------------- Task 5: SPI / flash errors during apply

def _force_return_false(rb):
    """The function returns 0 right at its first instruction: r0 = 0 and continue at lr. `resume <adr>` directly (dbg resume =
    halt + resume would discard the modified registers from the cache)."""
    lr = rb.reg("lr") & ~1
    rb.tcl("reg r0 0")
    rb.tcl(f"resume 0x{lr:08x}")


def _run_with_forced_failures(rb, should_fail, timeout=6.0):
    """A bp (already set before END) on a function: at every hit where should_fail(lr), the function returns false immediately. Ends after
    `timeout` without a hit. Returns the number of forced failures."""
    forced = 0
    try:
        while rb.wait_halt(timeout):
            rb.tcl("halt")  # openocd after a restart may not know about the halt: the registers would be empty
            lr = rb.reg("lr")
            if should_fail(lr):
                _force_return_false(rb)
                forced += 1
            else:
                rb.tcl("resume")
    finally:
        rb.disarm_all()
        if rb.halted():
            rb.t.resume()
    return forced


def _in_apply(rb):
    a = rb.bl_sym("boot::applyStagedImageIfPending")
    return lambda lr: a <= lr < a + 0x260


def test_remote_spi_read_error_transient(rb, base, link, remote_ok):
    """An SPI read during image verification fails once (the first trailer read in stagedImageGood): WriteError, a second attempt in the
    same boot applies the image. (The variant "fails in all 3 attempts" could not be forced reliably on the bench:
    resume after a bp on hla sometimes did not pass on the halt; it is not covered.)"""
    before, want = stage_halted_at_reboot(rb, link)
    rb.arm(rb.bl_sym("boot::w25q::read"))
    boot_bootloader(rb)
    in_apply = _in_apply(rb)
    forced = 0
    try:
        while forced == 0 and rb.wait_halt(20):
            rb.tcl("halt")
            if in_apply(rb.reg("lr")):
                _force_return_false(rb)
                forced = 1
            else:
                rb.tcl("resume")
    finally:
        rb.disarm_all()
        if rb.halted():
            rb.tcl("resume")
    assert forced == 1
    # Bench: after a forced failure the next bp in the same boot did not halt and the old image came up with pending = 1 (the next reset
    # applied it); "3 attempts in one boot" could thus not be verified. Requirement: never a lost or broken image.
    w = wait_app(rb, 40)
    assert w["kind"] == "app", w
    now = installed(rb)
    print(f"after one SPI read failure: installed {now} (before {before}, want {want})")
    if now == before:
        rb.reset()
    converged(rb, want)


def test_remote_bad_staged_image(rb, base, link, remote_ok):
    """An image byte is read wrongly during verification in the bootloader (flipped in s_buf before the CRC): BadImage -> pending cleared,
    the old application untouched."""
    before, _ = stage_halted_at_reboot(rb, link)
    rb.arm(rb.bl_sym("boot::Crc32::update"))
    boot_bootloader(rb)
    flipped = False
    try:
        while not flipped and rb.wait_halt(20):
            rb.tcl("halt")
            if rb.reg("r2") == 256:  # an s_buf chunk in stagedImageGood (the trailer CRC is 16 B)
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


def test_remote_program_error(rb, base, link, remote_ok):
    """flashprog::program fails once during the copy (the region is already erased): pending stays. Either a second attempt in the same boot
    completes the image, or the bootloader has no bootable application -> it releases PA1 (power) and waits; the next reset applies
    the image. Never a lost image or a hung core."""
    before, want = stage_halted_at_reboot(rb, link)
    rb.arm(rb.bl_sym("boot::flashprog::program"))
    boot_bootloader(rb)
    try:
        assert rb.wait_halt(30), "program() not reached"
        rb.tcl("halt")
        _force_return_false(rb)
    finally:
        rb.disarm_all()
        if rb.halted():
            rb.tcl("resume")
    time.sleep(12)
    w = rb.where()
    pa1 = bool(rb.u32(0x50000014) & 0x2)
    print(f"after one failed program(): {w}, PA1 {pa1}, installed {installed(rb, settle=1)}")
    assert w["kind"] in ("app", "bootloader"), w
    if w["kind"] == "bootloader":
        assert not pa1, "no bootable app but the power hold PA1 is still high"
        rb.reset()
    converged(rb, want)


# ------------------------------------------------------------------------- Task 6: interrupting a flash operation (T1)

SCRATCH = 0x0801F000
FRACTIONS = [0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 1.0, 1.1]


def _probe(rb):
    time.sleep(2.0)
    try:
        w = rb.where()
        return dict(where=w["kind"], pc=hex(w["pc"]), ipsr=w["ipsr"], eccr=hex(rb.eccr()), head=rb.flash_read(APP, 8).hex(),
                    pa1=bool(rb.u32(0x50000014) & 2))
    except Exception as e:  # noqa: BLE001 - the remote may have switched off (PA1)
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


F4B = ("F4b: interrupted program of the app vector page (no pending image) -> ECC double error -> remote bootloader NMI "
       "-> Default_Handler with PA1 still high (stays on, hung, battery drain)")


@pytest.mark.xfail(strict=True, raises=bootfi.ProductDefect, reason=F4B)
def test_remote_glitch_program_vector(rb, base, remote_ok):
    """Vector page erased and a reset during the programming of the 1st double-word, all from a single glitch routine from the application
    state (programming from a halted bootloader reports PGAERR|PGSERR, as on the base). Without a pending image: a bootloader
    (PA1 released) or the application is expected; never hung (ECC -> NMI, F4 on the base)."""
    img = IMG_A if installed(rb) == "A" else IMG_B
    w0, w1 = int.from_bytes(img[:4], "little"), int.from_bytes(img[4:8], "little")
    n_prog = rb.glitch(bootfi.OP_MEASURE_ERASE_THEN_PROGRAM, SCRATCH, w0=w0, w1=w1)
    assert n_prog < 0x80000000, f"flash error during measure: {n_prog:#x}"
    rb.glitch(bootfi.OP_MEASURE_ERASE, SCRATCH)
    print(f"program takes {n_prog} iterations")
    rows = _sweep(rb, "remote-program-vector", lambda: _restore_if_needed(rb), bootfi.OP_ERASE_THEN_PROGRAM, APP, n_prog,
                  w=(w0, w1))
    _restore_if_needed(rb)
    bad = [r for r in rows if r["where"] in ("off?", "unknown")]
    assert not bad, f"remote off/unknown after an interrupted program: {bad}"
    hangs = [r for r in rows if r["where"] == "hang"]
    if hangs:
        raise bootfi.ProductDefect(f"remote hung after an interrupted program: {hangs}")
