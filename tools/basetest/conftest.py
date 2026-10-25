"""Shared hardware session: USB-CDC link to the base (with automatic keepalive) and ST-Link helpers."""

import time

import pytest


def pytest_collection_modifyitems(config, items):
    # needs_user: only when the selection explicitly asks for them (-m needs_user); `-m hw` would otherwise select them too
    want_user = "needs_user" in (config.option.markexpr or "")
    for item in items:
        if not item.fspath.basename.endswith("_offline.py"):
            item.add_marker(pytest.mark.hw)
        if item.get_closest_marker("needs_user") and not want_user:
            item.add_marker(pytest.mark.skip(reason="needs a person at the bench: run with -m needs_user"))


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
    """Waits until pred() returns true; otherwise AssertionError with a description."""
    end = time.monotonic() + timeout
    while True:
        if pred():
            return True
        if time.monotonic() > end:
            raise AssertionError(f"timeout {timeout} s: {what or pred}")
        time.sleep(interval)


@pytest.fixture(scope="session")
def remote_probe(remote):
    """Remote via its ST-Link; skips the test when another process holds the probe (e.g. openocd of another session)."""
    import dbg
    holder = dbg.probe_holder("remote")
    if holder:
        pytest.skip(f"remote ST-Link held by another process: {holder[:120]}")
    return remote


KEEPALIVE_LOSS_MS = 6000  # lib/protocol constants.hpp


def remote_alive(base) -> bool:
    """Like router::isRemoteAlive() (LTO/--gc-sections drops it from the Release ELF, nobody calls it): link up,
    remote heard at some point and the last packet from the remote younger than KEEPALIVE_LOSS. Time = systick resolution_ * ticks_
    (duration, 1 ms resolution)."""
    res = base.u("stmcpp::systick::resolution_")
    now = (res * base.u("stmcpp::systick::ticks_")) & 0xFFFFFFFF
    last = base.u("router::_lastRemoteRx")
    age_ms = ((now - last) & 0xFFFFFFFF) / res
    return base.read_bool("Bluetooth::_connected") and base.read_bool("router::_remoteEverSeen") and age_ms < KEEPALIVE_LOSS_MS
