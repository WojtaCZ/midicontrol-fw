"""Zivotni cyklus spojeni (Task 7A): ztrata PC keepalive, reset base, vypadek BLE u remote, reset RN4870 na base,
a duvod NAK pro PC -> Remote, kdyz base nemuze predat (UNREACHABLE hned, BUSY jen pri zaplnene TX ceste).
Predpoklad: base a remote jsou sparovane (test_pairing.py). Remote se nevypina, jen se drzi jeho RN4870 v resetu."""

import time

import pytest

import proto
from conftest import remote_alive, wait_until
from proto import MT, NAK, Packet
from test_fault import boot, reboot
from test_log import keepalive_rtt, log  # noqa: F401 - fixture log
from test_relay import ADDR_BASE, ADDR_PC, ADDR_REMOTE, pc_signal, reply_to, to_remote

KEEPALIVE_LOSS_MS = 6000  # lib/protocol constants.hpp


def pc_alive(base) -> bool:
    """router::isPcAlive() (LTO ji vyhodi): _pcEverSeen a posledni paket z CDC mladsi nez KEEPALIVE_LOSS."""
    res = base.u("stmcpp::systick::resolution_")
    now = (res * base.u("stmcpp::systick::ticks_")) & 0xFFFFFFFF
    age_ms = ((now - base.u("router::_lastPcRx")) & 0xFFFFFFFF) / res
    return base.read_bool("router::_pcEverSeen") and age_ms < KEEPALIVE_LOSS_MS


def link_ready(base) -> bool:
    return base.read_bool("Bluetooth::_streamOpen") and base.read_bool("Bluetooth::_bonded") and remote_alive(base)


@pytest.fixture(scope="module")
def paired(base):
    wait_until(lambda: link_ready(base), 20, interval=0.5, what="remote link up (pair first: test_pairing.py)")
    return base


def test_pc_keepalive_loss(paired, link):
    base = paired
    keepalive_rtt(link)
    assert pc_alive(base)
    link.pause_keepalive(True)
    try:
        time.sleep(7.0)
        assert not pc_alive(base), "base still thinks the PC is alive after 7 s of silence"
    finally:
        link.pause_keepalive(False)
    link.drain()
    keepalive_rtt(link)
    wait_until(lambda: pc_alive(base), 2, what="PC alive again after traffic resumed")


def test_base_reset_cdc_returns(paired, link, log):  # noqa: F811
    base = paired
    log, t_gone = reboot(link, log, base.reset, gone_within=3.0, back_within=5.0)
    boot(log)
    keepalive_rtt(link)
    t0 = time.monotonic()
    wait_until(lambda: link_ready(base), 15, interval=0.5, what="remote link back within 15 s of reset")
    print(f"USB gone after {t_gone:.2f} s, remote link back {time.monotonic() - t0:.1f} s after CDC returned")
    log.close()


def test_remote_power_cycle_midsong(paired, link, remote_probe):
    base, remote = paired, remote_probe
    pc_signal(link, MT.SIGNAL_PLAYING, b"401")
    try:
        remote.hold_ble_reset(True)
        wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 12, what="base sees the remote gone")
        time.sleep(10.0)
    finally:
        remote.hold_ble_reset(False)
    wait_until(lambda: link_ready(base), 25, interval=0.5, what="remote link back after release")
    time.sleep(0.5)
    pc_signal(link, MT.SIGNAL_ANCHOR, b"Sloka 2\0")  # nasledujici SIGNAL_ANCHOR potvrdi remote
    pc_signal(link, MT.SIGNAL_STOPPED)


def test_rn4870_reset_on_base(paired, link):
    base = paired
    base.hold_ble_reset(True)
    time.sleep(0.05)
    base.hold_ble_reset(False)
    t0 = time.monotonic()
    time.sleep(1.0)
    wait_until(lambda: link_ready(base), 15, interval=0.5, what="link back within 15 s of RN4870 reset")
    print(f"link back {time.monotonic() - t0:.1f} s after the RN4870 pulse")
    keepalive_rtt(link)
    pc_signal(link, MT.SIGNAL_STOPPED)


def probe_nak(link, mtype, payload=b""):
    """PC -> Remote; (reply, doba [s]) nebo (None, ..) - reply = ACK/NAK od base (src 2) nebo remote (src 3)."""
    link.drain()
    pid = to_remote(link, mtype, payload)
    t = time.monotonic()
    r = link.recv(0.6, lambda p: p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
    return r, time.monotonic() - t


def check_naks(base, link, until, what, samples):
    """Posila SIGNAL_STOPPED a DIAG_GET dokud `until()`; kazdy NAK od base musi prijit do 200 ms a byt UNREACHABLE.
    BUSY je povolen jen kdyz je spojeni pouzitelne (pripojeno a stream otevreny; modul je chvilku v prikazovem rezimu)."""
    bad = []
    while not until():
        for mt, pl in ((MT.SIGNAL_STOPPED, b""), (MT.DIAG_GET, proto.diag_encode_get(0))):
            r, took = probe_nak(link, mt, pl)
            if r is not None and r.mtype == MT.NAK:
                samples.append((MT(mt).name, r.payload[1], round(took * 1000)))
                busy_ok = r.payload[1] == NAK.BUSY and base.read_bool("Bluetooth::_connected") and base.read_bool("Bluetooth::_streamOpen")
                if (r.payload[1] != NAK.UNREACHABLE and not busy_ok) or took > 0.2:
                    bad.append((MT(mt).name, r.payload.hex(), f"{took * 1000:.0f} ms"))
        time.sleep(0.1)
    assert not bad, f"{what}: NAK other than UNREACHABLE within 200 ms: {bad}"


def test_pc_to_remote_nak_reason_ble_down(paired, link, remote_probe):
    base, remote = paired, remote_probe
    samples = []
    try:
        remote.hold_ble_reset(True)
        # Base to vi az po %DISCONNECT% / ztrate spojeni; od te doby jen UNREACHABLE
        wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 12, what="base sees the remote gone")
        t_end = time.monotonic() + 6.0
        check_naks(base, link, lambda: time.monotonic() > t_end, "BLE down", samples)
        assert samples, "no NAK seen at all while the remote was away"
    finally:
        remote.hold_ble_reset(False)
    print("NAK samples (BLE down):", samples[:6], f"... n={len(samples)}")
    wait_until(lambda: link_ready(base), 25, interval=0.5, what="remote link back")


def test_pc_to_remote_nak_reason_after_base_reset(paired, link, log):  # noqa: F811
    base = paired
    samples = []
    log, _ = reboot(link, log, base.reset, gone_within=3.0, back_within=5.0)
    try:
        # od prvniho okamziku po novem CDC az do otevreni zabezpeceneho streamu
        t_end = time.monotonic() + 25
        check_naks(base, link, lambda: link_ready(base) or time.monotonic() > t_end, "after base reset", samples)
        assert samples, "no NAK seen while the link was coming back"
    finally:
        log.close()
    print("NAK samples (after reset):", samples[:6], f"... n={len(samples)}")
    wait_until(lambda: link_ready(base), 25, interval=0.5, what="remote link back")
