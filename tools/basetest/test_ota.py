"""OTA under running watchdogs (Task 7B): remote via the base, base via the CDC, base with a probe reset in the middle of the transfer.
The IWDG + loop watchdog run on both boards (6C/6D): this verifies that the bootloader and the application kick them during erase/staging.

The PC updater (../midicontrolpy-sw `midicontrol --cmd "update <base|remote> <file>"`) is built externally against the bench
RtMidi shim; the path is in $BASETEST_UPDATER (mandatory, no default). The updater holds the protocol CDC (flock/TIOCEXCL),
so the harness link is detached for the duration of the updater. The log CDC is held by the harness (the updater's LogPort points to a nonexistent file).
Remote: it restarts after the OTA; if it does not come back within 30 s, the test fails (and is NOT repeated). Files: BASETEST_REMOTE_BIN
(default ../midicontroller-fw/build/midicontroller-fw-update.bin), BASETEST_BASE_BIN (build/midicontrol-fw-update.bin)."""

import os
import re
import subprocess
import tempfile
import threading
import time

import pytest

import loglink
import proto
from proto import MT, Packet
import usblink
from conftest import remote_alive, wait_until
from test_log import BANNER_RE, PRE_END, PRE_START, wait_log_port  # noqa: F401
from test_remote_diag import ADDR_PC, ADDR_REMOTE, decode, get  # noqa: F401

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.abspath(os.path.join(HERE, "..", ".."))
REMOTE_FW = os.path.abspath(os.path.join(FW, "..", "midicontroller-fw"))
UPDATER = os.environ.get("BASETEST_UPDATER", "")
REMOTE_BIN = os.environ.get("BASETEST_REMOTE_BIN", os.path.join(REMOTE_FW, "build", "midicontroller-fw-update.bin"))
BASE_BIN = os.environ.get("BASETEST_BASE_BIN", os.path.join(FW, "build", "midicontrol-fw-update.bin"))
REMOTE_BACK_S = 30  # back within 30 s, otherwise BLOCKED
REMOTE_APP = 0x08004000  # after the 16K remote bootloader
REMOTE_FLASH_END = 0x08020000


def lsusb_ids() -> set:
    out = subprocess.run(["lsusb"], capture_output=True, text=True).stdout
    return set(re.findall(r"ID (abcd:123[45])", out))


def git_revision() -> str:
    with open(os.path.join(FW, "inc", "git.hpp")) as f:
        return re.search(r'revision = "([0-9a-f]+)"', f.read()).group(1)


class Updater:
    """Starts the updater as a subprocess (own XDG directories, WEB disabled, no log port); output to a file."""

    def __init__(self, target: str, binfile: str, tag: str):
        assert UPDATER and os.path.exists(UPDATER), "set BASETEST_UPDATER to the midicontrol binary built against the shim"
        assert os.path.exists(binfile), binfile
        self.tmp = tempfile.mkdtemp(prefix="ota-")
        os.chmod(self.tmp, 0o700)
        conf = os.path.join(self.tmp, "config.conf")
        with open(conf, "w") as f:
            f.write(f"[FILES]\nPlayFolder={self.tmp}\nRecordFolder={self.tmp}\n[WEB]\nEnabled=false\n[DIAG]\nLogPort={self.tmp}/none\nDir={self.tmp}/diag\n")
        env = dict(os.environ, XDG_RUNTIME_DIR=self.tmp, XDG_STATE_HOME=self.tmp)
        self.out_path = os.path.join(os.environ.get("BASETEST_OUT", self.tmp), f"updater-{tag}.log")
        self.out = open(self.out_path, "w")
        self.t0 = time.monotonic()
        self.p = subprocess.Popen([UPDATER, "--config", conf, "--cmd", f"update {target} {binfile}"],
                                  stdout=self.out, stderr=subprocess.STDOUT, env=env, cwd=self.tmp)

    def wait(self, timeout: float) -> int:
        try:
            rc = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.terminate()
            rc = self.p.wait(10)
            pytest.fail(f"updater did not finish in {timeout} s (see {self.out_path})")
        finally:
            self.out.close()
        self.duration = time.monotonic() - self.t0
        return rc

    def text(self) -> str:
        with open(self.out_path, errors="replace") as f:
            return f.read()

    def kill(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(10)
            except subprocess.TimeoutExpired:
                self.p.kill()
        self.out.close()


@pytest.fixture
def log():
    wait_log_port(5.0)
    L = loglink.LogLink()
    yield L
    err = L.error
    L.close()
    assert err is None, f"loglink thread failed: {err!r}"


def open_log_after_reboot(timeout=15.0):
    port = wait_log_port(timeout)
    for _ in range(50):
        try:
            return loglink.LogLink(port)
        except Exception:  # noqa: BLE001 - the port is not openable yet
            time.sleep(0.1)
    raise AssertionError("log CDC not openable")


def diag_try(link, cursor=0xFFFF, timeout=2.0):
    """A single DIAG_GET without an exception: (boot_id, head) or None when the ACK/DIAG_RESP did not arrive (nothing is swallowed silently)."""
    link.drain()
    pid = link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.DIAG_GET, proto.diag_encode_get(cursor)))
    reply = link.recv(timeout, lambda p: p.src == ADDR_REMOTE and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    if reply is None or reply.mtype != MT.ACK:
        return None
    resp = link.recv(timeout, lambda p: p.src == ADDR_REMOTE and p.dst == ADDR_PC and p.mtype == MT.DIAG_RESP)
    if resp is None:
        return None
    link.send(Packet(ADDR_PC, ADDR_REMOTE, 0, MT.ACK, bytes([resp.pid])))
    boot_id, head, _ = decode(resp)
    return boot_id, head


def diag_head(link):
    """(boot_id, head) on a stable link: at most one retry (R28), the count is printed; otherwise a failure."""
    r = diag_try(link)
    retries = 0
    if r is None:
        retries = 1
        print("diag_head: 1 retry (no ACK/DIAG_RESP at the first attempt)")
        r = diag_try(link)
    assert r is not None, "no DIAG_RESP after 1 retry"
    return r


def banner_and_cause(log, timeout=8.0):
    """Banner of the new boot (after the optional "log before reset" block) + the line with the reset cause."""
    first = log.lines(timeout, pred=lambda l: l == PRE_START or re.search(BANNER_RE, l))
    if first and first[-1] == PRE_START:
        log.lines(timeout, pred=lambda l: l == PRE_END)
        first = log.lines(timeout, pred=lambda l: re.search(BANNER_RE, l))
    assert first and re.search(BANNER_RE, first[-1]), first
    banner = first[-1]
    rest = log.lines(2.0)
    cause = next((l for l in rest if " I sys: reset: " in l), None)
    return banner, cause


def poll_usb_ids(stop: threading.Event, seen: set):
    while not stop.is_set():
        seen |= lsusb_ids()
        time.sleep(0.25)


@pytest.fixture
def base_recover(link, base):
    """Teardown of the base tests: if the base stayed in the bootloader (abcd:1235), BASETEST_BASE_BIN is flashed; then the link is restored and
    waits for abcd:1234 with both CDCs. Also releases debug (C_DEBUGEN)."""
    yield
    base.stop()
    if "abcd:1235" in lsusb_ids():
        print("RECOVERY: base left in the bootloader, flashing it back with the updater")
        up = Updater("base", BASE_BIN, "base-recover")
        try:
            rc = up.wait(300)
        finally:
            up.kill()
        assert rc == 0, up.text()[-500:]
    wait_until(lambda: "abcd:1234" in lsusb_ids(), 20, interval=0.3, what="abcd:1234 back (recovery)")
    link.detach()  # idempotent: closes any old port
    link.attach(settle=1.0)
    assert len(usblink._sysfs_ports()) == 2, usblink._sysfs_ports()


# ---------------------------------------------------------------------------------------------------------------------

def test_remote_ota_via_base(link, base, remote, log):
    """update remote <-update.bin> via the base CDC; the remote restarts, verify_image OK, code in flash = the uploaded image,
    the BOOT record (new boot_id) answers via DIAG, BLE link back (stream open + bonded + remote alive)."""
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen") and remote_alive(base), 15, what="remote link up first")
    boot_id0, _ = diag_head(link)
    base.stop()  # C_DEBUGEN cleared
    link.detach()
    log.skip()
    up = Updater("remote", REMOTE_BIN, "remote")
    try:
        rc = up.wait(600)
    finally:
        up.kill()
        link.attach(settle=0.0)
    txt = up.text()
    print(f"remote OTA: rc={rc} duration={up.duration:.1f} s")
    assert rc == 0, txt[-800:]
    assert "Firmware update complete" in txt, txt[-800:]
    # updater: CRC_OK from the remote (= verify_image OK on the remote) -> "Remote accepted the image"; no fallback "assuming success"
    assert "Remote accepted the image" in txt, txt[-800:]
    assert "assuming success" not in txt and "FAILED" not in txt, txt[-800:]
    # base log CDC: the base only forwards the remote OTA, it must neither reset nor log an error
    ota_lines = log.lines(1.0)
    print("base log during update:", ota_lines)
    assert not any(" E " in l[:20] or " I sys: reset: " in l for l in ota_lines), ota_lines
    assert not any(re.search(BANNER_RE, l) for l in ota_lines), "base rebooted during remote OTA"
    # positive proof: the log port stayed open the whole time (closing/opening the protocol port = detaching/attaching the harness link)
    assert any("usb: protocol port closed" in l for l in ota_lines) and any("usb: protocol port opened" in l for l in ota_lines), ota_lines

    # back within 30 s after the updater finishes: a new boot_id in DIAG (the remote restarts after verify), then the BLE link secured.
    # While the remote restarts DIAG cannot answer (expected, it is counted); after the boot_id changes at most 1 retry is allowed.
    t0 = time.monotonic()
    boot_id1 = None
    failed_while_rebooting = 0
    MAX_MISSES = 10
    while time.monotonic() - t0 < REMOTE_BACK_S:
        r = diag_try(link)
        if r is None:
            failed_while_rebooting += 1
            assert failed_while_rebooting <= MAX_MISSES, f"BLOCKED: {failed_while_rebooting} DIAG_GET without answer after the OTA"
        elif r[0] != boot_id0:
            boot_id1 = r[0]
            break
        time.sleep(0.5)
    back_s = time.monotonic() - t0
    print(f"DIAG attempts without answer while the remote rebooted: {failed_while_rebooting}")
    assert boot_id1 is not None, f"BLOCKED: remote did not come back with a new boot_id within {REMOTE_BACK_S} s"
    boot_id2, _ = diag_head(link)  # confirmation: DIAG answers after the restart (max 1 retry)
    assert boot_id2 == boot_id1, f"boot_id changed again: {boot_id1:#06x} -> {boot_id2:#06x}"
    print(f"remote boot_id {boot_id0:#06x} -> {boot_id1:#06x} {back_s:.1f} s after the updater returned")
    try:
        wait_until(lambda: base.read_bool("Bluetooth::_streamOpen") and remote_alive(base), REMOTE_BACK_S, interval=0.5,
                   what="remote link up after OTA")
        assert base.read_bool("Bluetooth::_bonded"), "link not secured/bonded"
    finally:
        base.stop()
    print(f"BLE link up and bonded {time.monotonic() - t0:.1f} s after the updater returned")
    reply, resp = get(link, ADDR_REMOTE, 0)
    assert resp is not None, "DIAG_RESP missing after OTA"

    # code: pc in the application + the first 8 KiB of the application flash = the image (without the 24 B trailer)
    with open(REMOTE_BIN, "rb") as f:
        img = f.read()
    n = 8192
    out = remote.tcl(f"capture {{halt}}; set pc [reg pc]; capture {{resume}}; return $pc")
    m = re.search(r"0x[0-9a-fA-F]{8}", out)
    remote.stop()
    assert m, out
    pc = int(m.group(0), 16)
    print(f"remote pc {pc:#010x}")
    assert REMOTE_APP <= pc < REMOTE_FLASH_END, f"remote pc {pc:#x} not in app"
    dump = os.path.join(up.tmp, "remote-flash.bin")
    remote.tcl(f"dump_image {dump} {REMOTE_APP:#x} {n}")
    remote.stop()
    with open(dump, "rb") as f:
        flash = f.read()
    assert flash == img[:n], "remote flash differs from the OTA image"
    assert not remote.core_state()["halted"]
    remote.stop()


def test_base_ota_over_cdc(link, base, log, base_recover):
    """update base: abcd:1235 during the transfer, then abcd:1234 with BOTH CDCs, a banner in the log with the new revision (= inc/git.hpp)."""
    base.stop()
    link.detach()
    log.close()
    seen: set = set()
    stop = threading.Event()
    th = threading.Thread(target=poll_usb_ids, args=(stop, seen), daemon=True)
    th.start()
    up = Updater("base", BASE_BIN, "base")
    try:
        rc = up.wait(300)
    finally:
        up.kill()
        stop.set()
        th.join(2)
    print(f"base OTA: rc={rc} duration={up.duration:.1f} s, usb ids seen {sorted(seen)}")
    assert rc == 0, up.text()[-800:]
    assert "abcd:1235" in seen, "bootloader (abcd:1235) never appeared"
    txt = up.text()
    assert "Image programmed" in txt and "Reconnected to the base" in txt, txt[-600:]
    wait_until(lambda: "abcd:1234" in lsusb_ids(), 15, interval=0.3, what="abcd:1234 back")
    L = open_log_after_reboot()
    try:
        banner, cause = banner_and_cause(L)
    finally:
        L.close()
    link.attach(settle=1.0)
    ports = usblink._sysfs_ports()
    assert len(ports) == 2, f"expected both CDC interfaces: {ports}"
    rev = re.search(r"rev ([0-9a-f]{7})", banner).group(1)
    print(f"banner: {banner}\ncause: {cause}")
    assert rev == git_revision()[:7], (rev, git_revision())
    assert cause is not None, "no reset cause line in the log"


def test_base_ota_power_loss(link, base, log, base_recover):
    """Probe reset of the base in the middle of the transfer -> it stays in the bootloader (abcd:1235, not :1234), a second update goes through."""
    base.stop()
    link.detach()
    log.close()
    up = Updater("base", BASE_BIN, "base-powerloss")
    try:
        wait_until(lambda: "abcd:1235" in lsusb_ids(), 60, interval=0.1, what="bootloader appeared")
        # the transfer is running: wait for the first signs of progress (updater output), then reset
        def progress() -> int:
            m = re.findall(r"(\d+)%\s+\((\d+)/(\d+)\)", up.text())
            return int(m[-1][0]) if m else 0
        wait_until(lambda: progress() > 0, 60, interval=0.1, what="transfer progress")
        time.sleep(1.0)
        assert up.p.poll() is None and 0 < progress() < 100, f"transfer not mid-way: {progress()}% {up.text()[-300:]}"
        print(f"reset at {progress()}%")
        t = time.monotonic()
        base.reset()
        base.stop()
        print(f"probe reset {time.monotonic() - t:.1f} s after progress +1 s; updater output so far:\n{up.text()[-300:]}")
        rc1 = up.wait(120)
    finally:
        up.kill()
    print(f"interrupted update: rc={rc1} after {up.duration:.1f} s")
    assert rc1 != 0, "interrupted update reported success"
    time.sleep(4.0)
    ids = lsusb_ids()
    print(f"usb after reset: {sorted(ids)}")
    assert "abcd:1235" in ids and "abcd:1234" not in ids, ids

    up2 = Updater("base", BASE_BIN, "base-second")
    try:
        rc2 = up2.wait(300)
    finally:
        up2.kill()
    print(f"second update: rc={rc2} duration={up2.duration:.1f} s")
    assert rc2 == 0, up2.text()[-800:]
    wait_until(lambda: "abcd:1234" in lsusb_ids(), 15, interval=0.3, what="abcd:1234 back after second update")
    L = open_log_after_reboot()
    try:
        banner, cause = banner_and_cause(L)
    finally:
        L.close()
    link.attach(settle=1.0)
    assert git_revision()[:7] in banner, banner
    print(f"banner: {banner}\ncause: {cause}")
