"""Sdilena hardwarova session: USB-CDC link k base (s automatickym keepalive) a ST-Link pomocnici."""

import time

import pytest


def pytest_collection_modifyitems(items):
    for item in items:
        if not item.fspath.basename.endswith("_offline.py"):
            item.add_marker(pytest.mark.hw)


@pytest.fixture(scope="session")
def link():
    import usblink
    L = usblink.Link(keepalive=True)
    yield L
    assert L.error is None, f"usblink thread failed: {L.error!r}"
    assert L.stray == b"", f"bytes outside frames on CDC: {L.stray!r}"
    L.close()


@pytest.fixture(scope="session")
def base():
    import dbg
    t = dbg.Target("base")
    yield t
    t.stop()


@pytest.fixture(scope="session")
def remote():
    import dbg
    t = dbg.Target("remote")
    yield t
    t.stop()


def wait_until(pred, timeout: float, interval: float = 0.2, what: str = "") -> bool:
    """Ceka, az pred() vrati pravdu; jinak AssertionError s popisem."""
    end = time.monotonic() + timeout
    while True:
        if pred():
            return True
        if time.monotonic() > end:
            raise AssertionError(f"timeout {timeout} s: {what or pred}")
        time.sleep(interval)


@pytest.fixture(scope="session")
def remote_probe(remote):
    """Remote pres jeho ST-Link; preskoci test, kdyz sondu drzi jiny proces (napr. openocd jine session)."""
    import dbg
    holder = dbg.probe_holder("remote")
    if holder:
        pytest.skip(f"remote ST-Link held by another process: {holder[:120]}")
    return remote


KEEPALIVE_LOSS_MS = 6000  # lib/protocol constants.hpp


def remote_alive(base) -> bool:
    """Jako router::isRemoteAlive() (LTO/--gc-sections ji z Release ELF vyhodi, nikdo ji nevola): spojeni,
    remote nekdy slyset a posledni paket od remote mladsi nez KEEPALIVE_LOSS. Cas = systick resolution_ * ticks_
    (duration, 1 ms rozliseni)."""
    res = base.u("stmcpp::systick::resolution_")
    now = (res * base.u("stmcpp::systick::ticks_")) & 0xFFFFFFFF
    last = base.u("router::_lastRemoteRx")
    age_ms = ((now - last) & 0xFFFFFFFF) / res
    return base.read_bool("Bluetooth::_connected") and base.read_bool("router::_remoteEverSeen") and age_ms < KEEPALIVE_LOSS_MS
