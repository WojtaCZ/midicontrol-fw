"""When detaching the probe the harness releases DHCSR.C_DEBUGEN (otherwise a real fault on the bench would end at a bkpt in fault::finish()
and with DBG_IWDG_STOP the IWDG would hang too). The DHCSR value before attaching is read from the log of a new openocd (-d3): examine reads it
as the first thing and only then sets C_DEBUGEN itself (Target.dhcsr_on_attach)."""

import pytest

import dbg

C_DEBUGEN, S_HALT = 1 << 0, 1 << 17


@pytest.fixture(params=["base", "remote"])
def target(request):
    if request.param == "remote" and dbg.probe_holder("remote"):
        pytest.skip("remote ST-Link held by another process")
    t = dbg.Target(request.param)
    yield t
    t.stop()


def test_stop_releases_debugen(target):
    assert target.core_state()["dhcsr"] & C_DEBUGEN, "attached session should have C_DEBUGEN set"
    target.stop()
    d = target.dhcsr_on_attach()
    assert not d & C_DEBUGEN, f"C_DEBUGEN still set after Target.stop(): DHCSR 0x{d:08x}"
    assert not d & S_HALT, f"core halted after Target.stop(): DHCSR 0x{d:08x}"


def test_stop_all_atexit_releases_debugen(target):
    target.core_state()
    dbg.stop_all()  # the atexit path
    d = target.dhcsr_on_attach()
    assert not d & C_DEBUGEN, f"C_DEBUGEN still set after stop_all(): DHCSR 0x{d:08x}"
    assert not d & S_HALT


def test_halted_core_is_resumed_and_released(target):
    """A halted core (as after a bkpt) is resumed on release and C_DEBUGEN is cleared."""
    target.tcl("halt")
    assert target.core_state()["halted"]
    target.stop()
    d = target.dhcsr_on_attach()
    assert not d & (C_DEBUGEN | S_HALT), f"DHCSR 0x{d:08x}"
