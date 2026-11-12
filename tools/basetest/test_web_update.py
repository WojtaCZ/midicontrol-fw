"""FAULTS 2.1 / SYS-SW web firmware update: the status page's upload (POST /api/firmware/<base|remote>) on the bench.

The PC service (`midicontrol --headless`, the same $BASETEST_UPDATER binary as test_ota.py) runs with its own config and XDG
dirs, [WEB] on 127.0.0.1:$BASETEST_WEB_PORT (default 28131, inside the container's published range), no log port. It holds
the protocol CDC, so the harness link is detached while it runs; the log CDC stays free for the banner check.

- test_web_base_update_repeated: three base updates in ONE service run. 2.1 was the service not reopening the base after
  the first web update (openWhenReady fix); each update must finish "done", the base must come back as abcd:1234 with both
  CDCs, the new boot must print the banner with inc/git.hpp's revision, and the service must reconnect by itself (the next
  POST is accepted).
- test_web_remote_update: one remote update through the web; done, then the BLE link back.
- test_web_busy: while a base update runs, a second POST gets 409 "already running", the console client's `update` says
  busy, and the page and /api/status keep answering (a page reload during the transfer).
- test_web_base_reset_mid_transfer: the base is reset by the probe mid-transfer (the nearest the bench gets to an unplug):
  the job must end "failed" (never hang in "running"), the base stays in its bootloader, and the CLI updater recovers it.
"""

import json
import os
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request

import pytest

import usblink
from conftest import remote_alive, wait_until
from test_ota import (BASE_BIN, REMOTE_BIN, UPDATER, Updater, banner_and_cause, base_recover, git_revision,  # noqa: F401
                      lsusb_ids, open_log_after_reboot, poll_usb_ids)

PORT = int(os.environ.get("BASETEST_WEB_PORT", "28131"))
BASE_UPDATE_S = 300
REMOTE_UPDATE_S = 600
REPEATS = int(os.environ.get("BASETEST_WEB_REPEATS", "3"))


class Service:
    """The headless PC service with the web server on; output to a file."""

    def __init__(self, tag: str):
        assert UPDATER and os.path.exists(UPDATER), "set BASETEST_UPDATER to the midicontrol binary built against the shim"
        self.tmp = tempfile.mkdtemp(prefix="web-")
        os.chmod(self.tmp, 0o700)
        self.conf = os.path.join(self.tmp, "config.conf")
        with open(self.conf, "w") as f:
            f.write(f"[FILES]\nPlayFolder={self.tmp}\nRecordFolder={self.tmp}\n"
                    f"[WEB]\nEnabled=true\nPort={PORT}\nBind=127.0.0.1\n"
                    f"[DIAG]\nLogPort={self.tmp}/none\nDir={self.tmp}/diag\n")
        self.env = dict(os.environ, XDG_RUNTIME_DIR=self.tmp, XDG_STATE_HOME=self.tmp)
        self.out_path = os.path.join(os.environ.get("BASETEST_OUT", self.tmp), f"service-{tag}.log")
        self.out = open(self.out_path, "w")
        self.p = subprocess.Popen([UPDATER, "--config", self.conf, "--headless"], stdin=subprocess.DEVNULL,
                                  stdout=self.out, stderr=subprocess.STDOUT, env=self.env, cwd=self.tmp)
        wait_until(self.answers, 10, interval=0.2, what=f"web server on 127.0.0.1:{PORT}")

    def request(self, method: str, path: str, body: bytes | None = None, timeout: float = 30.0):
        """(status, body bytes); HTTP errors are returned, not raised."""
        req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", "application/octet-stream")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def answers(self) -> bool:
        assert self.p.poll() is None, f"service exited rc={self.p.returncode}: {self.text()[-600:]}"
        try:
            return self.request("GET", "/api/status", timeout=2)[0] == 200
        except OSError:
            return False

    def fields(self) -> dict:
        st, body = self.request("GET", "/api/status")
        assert st == 200, (st, body[:200])
        return json.loads(body)["nodes"]["software"]["fields"]

    def post(self, target: str, binfile: str):
        with open(binfile, "rb") as f:
            return self.request("POST", f"/api/firmware/{target}", f.read(), timeout=60)

    def post_when_ready(self, target: str, binfile: str, timeout: float = 30.0) -> float:
        """POST until the service accepts it (202); 409 "the base is not connected" is retried (the service is still
        (re)connecting), any other answer fails. Returns the seconds it took."""
        t0 = time.monotonic()
        while True:
            st, body = self.post(target, binfile)
            if st == 202:
                return time.monotonic() - t0
            assert st == 409 and b"not connected" in body, (st, body)
            assert time.monotonic() - t0 < timeout, f"service did not reconnect to the base within {timeout} s: {body}"
            time.sleep(0.5)

    def wait_job(self, target: str, timeout: float, during=None) -> dict:
        """Polls /api/status until the job for `target` leaves "running"; `during(fields)` runs on each running poll."""
        t0 = time.monotonic()
        seen_running = False
        while time.monotonic() - t0 < timeout:
            f = self.fields()
            if f.get("fw_update_target") == target and f.get("fw_update_state") == "running":
                seen_running = True
                if during:
                    during(f)
            elif seen_running or (f.get("fw_update_target") == target and f.get("fw_update_state") in ("done", "failed")):
                return f
            time.sleep(0.5)
        pytest.fail(f"web update of the {target} still running after {timeout} s: {self.fields()} ({self.out_path})")

    def cli(self, cmd: str, timeout: float = 30.0) -> subprocess.CompletedProcess:
        """The console client attached to this service (same XDG_RUNTIME_DIR -> same socket)."""
        return subprocess.run([UPDATER, "--config", self.conf, "--cmd", cmd], capture_output=True, text=True,
                              env=self.env, cwd=self.tmp, timeout=timeout)

    def text(self) -> str:
        self.out.flush()
        with open(self.out_path, errors="replace") as f:
            return f.read()

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait(5)
        self.out.close()
        if "BASETEST_OUT" in os.environ:  # the log lives there; the temp dir holds nothing else worth keeping
            shutil.rmtree(self.tmp, ignore_errors=True)


@pytest.fixture
def service(request, link, base, base_recover):  # noqa: F811 - base_recover tears down after the service
    base.stop()
    link.detach()
    s = Service(request.node.name.replace("[", "-").replace("]", ""))
    yield s
    s.stop()
    print(f"service log: {s.out_path}")


def check_base_back():
    """abcd:1234 again with the new banner (inc/git.hpp revision) and both CDCs."""
    wait_until(lambda: "abcd:1234" in lsusb_ids(), 20, interval=0.3, what="abcd:1234 back")
    L = open_log_after_reboot()
    try:
        banner, cause = banner_and_cause(L)
    finally:
        L.close()
    ports = usblink._sysfs_ports()
    assert len(ports) == 2, f"expected both CDC interfaces: {ports}"
    assert git_revision()[:7] in banner, (banner, git_revision())
    return banner, cause


def test_web_base_update_repeated(service):
    for i in range(REPEATS):
        seen: set = set()
        stop = threading.Event()
        th = threading.Thread(target=poll_usb_ids, args=(stop, seen), daemon=True)
        th.start()
        try:
            waited = service.post_when_ready("base", BASE_BIN)
            t0 = time.monotonic()
            f = service.wait_job("base", BASE_UPDATE_S)
        finally:
            stop.set()
            th.join(2)
        print(f"web base update {i + 1}/{REPEATS}: accepted after {waited:.1f} s, {f['fw_update_state']} "
              f"in {time.monotonic() - t0:.1f} s, usb ids seen {sorted(seen)}, msg {f.get('fw_update_msg')!r}")
        assert f["fw_update_state"] == "done", f"{f} (service log {service.out_path})"
        assert "abcd:1235" in seen, "bootloader (abcd:1235) never appeared"
        banner, cause = check_base_back()
        print(f"  {banner} / {cause}")


def test_web_remote_update(service, base):
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen") and remote_alive(base), 15, what="remote link up first")
    base.stop()
    service.post_when_ready("remote", REMOTE_BIN)
    t0 = time.monotonic()
    marks = {}

    def during(f):
        pct = int(f.get("fw_update_pct") or 0)
        for m in (1, 50, 100):
            if pct >= m and m not in marks:
                marks[m] = round(time.monotonic() - t0, 1)

    f = service.wait_job("remote", REMOTE_UPDATE_S, during)
    print(f"web remote update: {f['fw_update_state']} in {time.monotonic() - t0:.1f} s (s to pct: {marks}), "
          f"msg {f.get('fw_update_msg')!r}")
    assert f["fw_update_state"] == "done", f"{f} (service log {service.out_path})"
    try:
        # the remote reboots into its bootloader after accepting the image: the old link must drop first, otherwise
        # the "link up" below would only be the link from before the update
        wait_until(lambda: not base.read_bool("Bluetooth::_streamOpen"), 20, interval=0.2,
                   what="link down while the remote reboots")
        print(f"link down {time.monotonic() - t0:.1f} s after the POST")
        wait_until(lambda: base.read_bool("Bluetooth::_streamOpen") and remote_alive(base), 60, interval=0.5,
                   what="remote link up after the web OTA")
        assert base.read_bool("Bluetooth::_bonded"), "link not secured/bonded"
    finally:
        base.stop()
    print(f"remote link up {time.monotonic() - t0:.1f} s after the POST")


def test_web_busy(service):
    service.post_when_ready("base", BASE_BIN)
    checks = {"post": None, "cli": None, "page": 0}

    def during(f):
        if checks["post"] is None:
            checks["post"] = service.post("remote", REMOTE_BIN)
        if checks["cli"] is None:
            r = service.cli(f"update base {BASE_BIN}")
            checks["cli"] = (r.returncode, r.stdout + r.stderr)
        st, body = service.request("GET", "/")
        assert st == 200 and b"<html" in body.lower(), (st, body[:100])
        checks["page"] += 1

    f = service.wait_job("base", BASE_UPDATE_S, during)
    print(f"busy checks: {checks}, job {f['fw_update_state']}")
    assert checks["post"] is not None, "the update finished before the busy POST could be sent"
    st, body = checks["post"]
    assert st == 409 and b"already running" in body, (st, body)
    rc, out = checks["cli"]
    assert rc != 0 and "busy" in out, (rc, out)
    assert checks["page"] >= 2, "page not reloaded during the transfer"
    assert f["fw_update_state"] == "done", f
    check_base_back()


def test_web_base_reset_mid_transfer(service, base):
    service.post_when_ready("base", BASE_BIN)
    reset = {"at": None}

    def during(f):
        if reset["at"] is None and 5 <= int(f.get("fw_update_pct") or 0) < 90:
            reset["at"] = f["fw_update_pct"]
            base.reset()
            base.stop()

    f = service.wait_job("base", 180, during)
    print(f"reset at {reset['at']}%: job {f['fw_update_state']}, msg {f.get('fw_update_msg')!r}")
    assert reset["at"] is not None, f"transfer never seen mid-way: {f}"
    assert f["fw_update_state"] == "failed", f
    assert service.answers(), "service stopped answering after the failed update"
    time.sleep(4.0)
    ids = lsusb_ids()
    assert "abcd:1235" in ids and "abcd:1234" not in ids, ids
    service.stop()  # frees the bootloader port for the CLI updater
    up = Updater("base", BASE_BIN, "web-reset-recover")
    try:
        rc = up.wait(BASE_UPDATE_S)
    finally:
        up.kill()
    assert rc == 0, up.text()[-800:]
    check_base_back()
