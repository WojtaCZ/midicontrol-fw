"""R21: mrtvy RN4870 base neresetuje (smycka ~4,5 s), ale zaloguje `E ble:` a zapise ERROR jednou, bezi dal bez BLE a po 60 s
zkusi init znovu. Modul se drzi v resetu zapisem PA1 = 0 pres sondu za behu jadra (Target.hold_ble_reset); base init() PA1 po
resetu sam zvedne, proto se zapis opakuje ve vlakne. Po uvolneni PA1 se BLE vrati nejpozdeji pri 60s pokusu."""

import re
import struct
import threading
import time
import zlib

import pytest

import dbg
from conftest import remote_alive, wait_until
from test_fault import boot, reboot
from test_log import BANNER_RE, keepalive_rtt, log  # noqa: F401 - fixture log

ERR_BLE_INIT = 1


class Holder(threading.Thread):
    def __init__(self, base):
        super().__init__(daemon=True)
        self.base, self.stop_evt, self.error = base, threading.Event(), None

    def run(self):
        try:
            while not self.stop_evt.is_set():
                self.base.hold_ble_reset(True)
                time.sleep(0.03)
        except Exception as e:  # noqa: BLE001
            self.error = e

    def release(self):
        self.stop_evt.set()
        self.join(5)


def ticks(base) -> int:
    return base.u("stmcpp::systick::ticks_")


def test_dead_rn4870_no_reset_loop_and_retry(base, link, log):  # noqa: F811
    base.write(base.sym("fault::_record"), bytes(dbg.FAULT_RECORD_SIZE))  # zadny starsi zaznam
    holder = Holder(base)
    try:
        def action():
            holder.start()
            base.reset()

        log, _ = reboot(link, log, action, gone_within=3.0)
        # Boot: banner a (po ~2-3 s) selhani init; pak >= 15 s behu
        _, lines = boot(log)
        lines += log.lines(3.0)
        t0 = ticks(base)
        for _ in range(5):  # 15 s: USB zije, zadny dalsi boot
            lines += log.lines(3.0)
            keepalive_rtt(link)
        t1 = ticks(base)
        assert holder.error is None, holder.error
        assert t1 - t0 >= 15000, f"uptime did not grow ({t0} -> {t1}): base reset?"
        assert sum(bool(re.search(BANNER_RE, l)) for l in lines) == 1, "second boot (reset loop)"
        ble_e = [l for l in lines if re.search(r" E ble:", l)]
        assert len(ble_e) == 1, ble_e
        assert "RN4870 init failed" in ble_e[0]

        # Zaznam ERROR/ERR_BLE_INIT, jednou, uz oznameny (dalsi banner ho nevydava za chybu minuleho behu)
        rec = base.fault_record()
        assert rec and rec["kind"] == 5 and rec["code"] == ERR_BLE_INIT and rec["count"] == 1, rec
        raw = base.read(base.sym("fault::_record"), dbg.FAULT_RECORD_SIZE)
        assert struct.unpack_from("<I", raw, 88)[0] == zlib.crc32(raw[:88]) & 0xFFFFFFFF
        assert base.u("fault::_announcedCrc") == struct.unpack_from("<I", raw, 88)[0], "record not marked announced"
        assert not base.read_bool("Bluetooth::_connected")
        assert not base.core_state()["halted"]

        # Uvolnit PA1: modul nabehne; BLE ale zapne az 60s retry (base ho znovu resetuje a nakonfiguruje)
        holder.release()
        base.hold_ble_reset(False)
        retry = log.lines(75.0, pred=lambda l: "RN4870 init ok after retry" in l)
        assert retry and "RN4870 init ok after retry" in retry[-1], retry[-3:]
        wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 25, what="base reconnected to the remote")
        wait_until(lambda: base.read_bool("Bluetooth::_bonded"), 5, what="link secured")
        wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
        keepalive_rtt(link)
    finally:
        holder.release()
        try:
            base.hold_ble_reset(False)
        except Exception:  # noqa: BLE001
            pass
