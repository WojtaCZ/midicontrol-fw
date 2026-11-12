"""SYS-PC-03 / D1: on PC present -> PC lost the base sends All Notes Off to DIN and switches the SSR off, once.

The keepalive pause stands in for a PC that hangs; KEEPALIVE_LOSS is 6 s (lib/protocol constants.hpp).
D19 (no PC ever seen keeps power) needs a base that never heard the PC: covered by the host test PcLossGuard.
"""

import time

from conftest import KEEPALIVE_LOSS_MS, wait_until
from proto import MT
from test_cdc_local import checkbox_power
from test_log import log  # noqa: F401 - fixture log


def test_pc_loss_cuts_organ_power(link, base, log):
    # a previous pytest run may have just left the base in "PC lost": wait for this session's keepalives
    wait_until(lambda: base.read_bool("router::_pcAliveLogged"), 8.0, what="PC alive")
    link.send_msg(MT.SET_CURRENT, b"\x01")
    wait_until(lambda: base.gpio("A", 4) == 1, 2.0, what="SSR on after SET_CURRENT 1")
    log.lines(0.5)  # the log port replays its ring buffer on open (an earlier loss may be in it): let it arrive
    log.skip()  # then only lines from now on
    link.pause_keepalive(True)
    try:
        log.wait_for(r"PC lost: all notes off, organ power off", KEEPALIVE_LOSS_MS / 1000 + 3)
        assert base.gpio("A", 4) == 0, "SSR still on after PC loss"
        assert checkbox_power(base) == 0, "menu power checkbox not synced"
    finally:
        link.pause_keepalive(False)
    # PC back: the SSR stays off until the PC asks for it, and the cut-off does not repeat
    wait_until(lambda: base.read_bool("router::_pcAliveLogged"), 5.0, what="PC alive again")
    assert base.gpio("A", 4) == 0
    assert not [l for l in log.lines(2.0) if "PC lost: all notes off" in l], "cut-off repeated after the PC returned"
    link.send_msg(MT.SET_CURRENT, b"\x00")
