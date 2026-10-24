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
from proto import ADDR_BASE, ADDR_PC, ADDR_REMOTE, MT, NAK
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
        # >= 70 s s drzenym RST: pokryje aspon jeden 60s retry (restart modulu musi byt neblokujici). KEEPALIVE
        # odpovida router::process() z hlavni smycky, takze RTT = perioda smycky (zadne zmrazeni USB/protokolu).
        rtts, missed = [], 0
        t_end = time.monotonic() + 72.0
        while time.monotonic() < t_end:
            t = time.monotonic()
            link.send_msg(MT.KEEPALIVE)
            if link.recv(1.0, lambda p: p.mtype == MT.KEEPALIVE_ACK) is None:
                missed += 1
            else:
                rtts.append(time.monotonic() - t)
            lines += log.lines(0.05)
        assert missed == 0, f"{missed} KEEPALIVE without answer"
        assert max(rtts) < 0.2, f"max KEEPALIVE RTT {max(rtts) * 1000:.0f} ms over {len(rtts)} samples"
        t1 = ticks(base)
        assert holder.error is None, holder.error
        assert t1 - t0 >= 70000, f"uptime did not grow ({t0} -> {t1}): base reset?"
        assert sum(bool(re.search(BANNER_RE, l)) for l in lines) == 1, "second boot (reset loop)"
        ble_e = [l for l in lines if re.search(r" E ble:", l)]
        assert len(ble_e) == 1, ble_e
        assert "RN4870 init failed" in ble_e[0]
        assert any("RN4870 init failed again" in l for l in lines), "no 60 s retry seen"

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


def test_wedged_module_detected_and_restarted(base, link, log):  # noqa: F811
    """B: modul, ktery uz pracoval a pak mlci (RST dole), se pozna podle 3 po sobe jdoucich timeoutu `$$$` -> BLE down (E ble:
    jednou), po uvolneni RST ho 60s retry restartuje a nakonfiguruje (remote se znovu pripoji)."""
    base.write(base.sym("fault::_record"), bytes(dbg.FAULT_RECORD_SIZE))
    log, _ = reboot(link, log, base.reset, gone_within=3.0)
    boot(log)
    wait_until(lambda: base.read_bool("Bluetooth::_bonded") and remote_alive(base), 25, interval=0.5, what="base up and linked after reset")
    holder = Holder(base)
    try:
        holder.start()
        time.sleep(0.5)
        lines = []
        # Tri po sobe jdouci timeouty `$$$` (modul v resetu neodpovi); kazdy trva ~1 s
        for _ in range(4):
            base.call('Bluetooth::queueCommand("Z")')
            lines += log.lines(2.5)
        lines += log.lines(2.0)
        ble_e = [l for l in lines if " E ble:" in l]
        assert len(ble_e) == 1 and "not responding" in ble_e[0], (ble_e, [l for l in lines if "ble:" in l])
        assert len([l for l in lines if "$$$ timeout" in l]) >= 3
        assert not base.core_state()["halted"]
        keepalive_rtt(link)  # USB bezi dal bez BLE
        # Final review M1: mrtvy modul -> spojeni neplatí hned (ne az za 60 s), PC dostane NAK UNREACHABLE
        assert not base.read_bool("Bluetooth::_connected"), "link flags stale after the wedge"
        assert not base.read_bool("Bluetooth::_streamOpen")
        assert not base.read_bool("Bluetooth::_peerAccepted")
        pid = link.send_msg(MT.SIGNAL_STOPPED, dst=ADDR_REMOTE)
        nak = link.recv(1.0, lambda p: p.src == ADDR_BASE and p.dst == ADDR_PC and p.mtype == MT.NAK and p.payload[:1] == bytes([pid]))
        assert nak is not None and nak.payload == bytes([pid, NAK.UNREACHABLE]), nak

        holder.release()
        base.hold_ble_reset(False)
        retry = log.lines(75.0, pred=lambda l: "RN4870 init ok after retry" in l)
        assert retry and "RN4870 init ok after retry" in retry[-1], retry[-3:]
        wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 25, what="base reconnected to the remote")
        wait_until(lambda: base.read_bool("Bluetooth::_bonded"), 8, what="link secured")
        wait_until(lambda: remote_alive(base), 8, interval=0.5, what="remote alive")
    finally:
        holder.release()
        try:
            base.hold_ble_reset(False)
        except Exception:  # noqa: BLE001
            pass
