"""OTA pod bezicimi watchdogy (Task 7B): remote pres base, base pres CDC, base s resetem sondou uprostred prenosu.
IWDG + smyckovy watchdog bezi na obou deskach (6C/6D): ověřuje se, ze bootloader a aplikace pri mazani/staging kopou.

PC updater (../midicontrolpy-sw `midicontrol --cmd "update <base|remote> <file>"`) se zvenci sestavuje proti lavicovemu
RtMidi shimu; cesta v $BASETEST_UPDATER (jinak scratchpad pc6e3/b/midicontrol). Updater drzi protokolovy CDC (flock/TIOCEXCL),
proto se harness link na dobu updateru odpoji. Log CDC drzi harness (LogPort updateru ukazuje na neexistujici soubor).
Remote: po OTA se restartuje; nevrati-li se do 30 s, test selze (a NEOPAKUJE se). Soubory: BASETEST_REMOTE_BIN
(default ../midicontroller-fw/build/midicontroller-fw-update.bin), BASETEST_BASE_BIN (build/midicontrol-fw-update.bin)."""

import os
import re
import subprocess
import tempfile
import threading
import time

import pytest

import loglink
import usblink
from conftest import remote_alive, wait_until
from test_log import BANNER_RE, PRE_END, PRE_START, wait_log_port  # noqa: F401
from test_remote_diag import ADDR_REMOTE, decode, get, head_and_boot  # noqa: F401

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.abspath(os.path.join(HERE, "..", ".."))
REMOTE_FW = os.path.abspath(os.path.join(FW, "..", "midicontroller-fw"))
UPDATER = os.environ.get("BASETEST_UPDATER", "")
REMOTE_BIN = os.environ.get("BASETEST_REMOTE_BIN", os.path.join(REMOTE_FW, "build", "midicontroller-fw-update.bin"))
BASE_BIN = os.environ.get("BASETEST_BASE_BIN", os.path.join(FW, "build", "midicontrol-fw-update.bin"))
REMOTE_BACK_S = 30  # zpet do 30 s, jinak BLOCKED
REMOTE_APP = 0x08004000  # za 16K bootloaderem remote
REMOTE_FLASH_END = 0x08020000


def lsusb_ids() -> set:
    out = subprocess.run(["lsusb"], capture_output=True, text=True).stdout
    return set(re.findall(r"ID (abcd:123[45])", out))


def git_revision() -> str:
    with open(os.path.join(FW, "inc", "git.hpp")) as f:
        return re.search(r'revision = "([0-9a-f]+)"', f.read()).group(1)


class Updater:
    """Spusti updater jako podproces (vlastni XDG adresare, WEB vypnuty, bez log portu); vystup do souboru."""

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
        except Exception:  # noqa: BLE001 - port jeste neni otevritelny
            time.sleep(0.1)
    raise AssertionError("log CDC not openable")


def diag_head(link, tries=4):
    """head_and_boot s opakovanim: prvni DIAG_GET po delsi pauze nekdy zustane bez DIAG_RESP (remote ho po ACK posila pozdeji)."""
    for i in range(tries):
        try:
            return head_and_boot(link)
        except AssertionError:
            if i == tries - 1:
                raise
            time.sleep(0.5)


def banner_and_cause(log, timeout=8.0):
    """Banner noveho bootu (po pripadnem bloku "log before reset") + radek s pricinou resetu."""
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


# ---------------------------------------------------------------------------------------------------------------------

def test_remote_ota_via_base(link, base, remote, log):
    """update remote <-update.bin> pres base CDC; remote se restartuje, verify_image OK, kod v flash = nahrany obraz,
    BOOT zaznam (novy boot_id) odpovida pres DIAG, BLE spojeni zpet (stream open + bonded + remote alive)."""
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen") and remote_alive(base), 15, what="remote link up first")
    boot_id0, _ = diag_head(link)
    base.stop()  # C_DEBUGEN pryc
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
    assert not re.search(r"FAILED|error", txt, re.I) or "verify" in txt.lower(), txt[-800:]
    ota_lines = [l for l in log.lines(0.5)]
    print("base log lines during update:", len(ota_lines))

    # zpet do 30 s po konci updateru: novy boot_id v DIAG (remote se po verify restartuje), pak BLE spojeni zabezpecene
    t0 = time.monotonic()
    boot_id1 = None
    while time.monotonic() - t0 < REMOTE_BACK_S:
        try:
            bid, _ = head_and_boot(link)
        except AssertionError:
            time.sleep(0.5)
            continue
        if bid != boot_id0:
            boot_id1 = bid
            break
        time.sleep(0.5)
    back_s = time.monotonic() - t0
    assert boot_id1 is not None, f"BLOCKED: remote did not come back with a new boot_id within {REMOTE_BACK_S} s"
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

    # kod: pc v aplikaci + prvnich 8 KiB flash aplikace = obraz (bez 24 B trailer)
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


def test_base_ota_over_cdc(link, base, log):
    """update base: abcd:1235 behem prenosu, pak abcd:1234 se OBEMA CDC, banner v logu s novou revizi (= inc/git.hpp)."""
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


def test_base_ota_power_loss(link, base, log):
    """Reset base sondou uprostred prenosu -> zustane v bootloaderu (abcd:1235, ne :1234), druhy update projde."""
    base.stop()
    link.detach()
    log.close()
    up = Updater("base", BASE_BIN, "base-powerloss")
    try:
        wait_until(lambda: "abcd:1235" in lsusb_ids(), 60, interval=0.1, what="bootloader appeared")
        # prenos bezi: pockat na prvni znamky postupu (vystup updateru), pak reset
        wait_until(lambda: re.search(r"\d+\s*%|chunk|sent|block|page", up.text(), re.I) is not None, 60, interval=0.1,
                   what="transfer progress")
        time.sleep(1.0)
        assert up.p.poll() is None, f"updater already finished: {up.text()[-400:]}"
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
