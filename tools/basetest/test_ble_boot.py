"""FAULTS 2.2 / SYS-PWR-02: after a base boot the BLE link comes up on the first try, without a counted loss or ERR_CONN.

Bench 2026-11-12: the remote still holds the link to the base that just reset; it loses it ~2 s into the base boot and restarts
its advertising ($$$ IA/A commands, blocking). That collides with the base's first CE (~5.0 s): %CONNECT% then %ERR_CONN%
~120 ms later, before %STREAM_OPEN%, and the module connects again by itself ~200 ms later. That is a failed connection setup,
not a lost link (linkstats::Stats::onLost returns false before STREAM_OPEN; host test LinkStats.SetupFailureBeforeStreamOpenIsNotALoss).
Before the fix every boot counted losses=1 errConn=1.
"""

import time

import pytest

from conftest import remote_alive, wait_until
from test_log import boot_section, log, reset_and_reopen  # noqa: F401 - fixture log

BOOTS = 3
LINK_UP_S = 10.0  # SYS-PWR-02: link within 6-8 s of power-on, bench ~7.4 s to STREAM_OPEN; margin for the log CDC reopen


def counters(base) -> dict:
    raw = base.read(base.sym("Bluetooth::_stats"), 10)
    v = [int.from_bytes(raw[i:i + 2], "little") for i in range(0, 10, 2)]
    return dict(zip(("losses", "errConn", "reboots", "secureRetries", "lastReconnectMs"), v))


@pytest.mark.parametrize("boot", range(BOOTS))
def test_boot_link_without_counted_loss(link, base, log, boot):
    wait_until(lambda: remote_alive(base), 15.0, what="remote alive before the reset (the remote must hold a link)")
    t0 = time.monotonic()
    log = reset_and_reopen(link, log, base.reset)
    try:
        boot_section(log)  # the log port replays the previous boot ("log before reset") with its own stream open first
        log.wait_for(r"I ble: stream open", LINK_UP_S - (time.monotonic() - t0))
        wait_until(lambda: remote_alive(base), 5.0, what="remote alive after the boot")
        print(f"boot {boot}: link up {time.monotonic() - t0:.1f} s after the reset, {counters(base)}")
        c = counters(base)
        assert c["losses"] == 0 and c["errConn"] == 0, f"a loss was counted during the boot link setup: {c}"
    finally:
        log.close()
