"""Harness pri odpojeni sondy uvolni DHCSR.C_DEBUGEN (jinak by skutecny fault na lavici skoncil na bkpt v fault::finish()
a s DBG_IWDG_STOP by viselo i IWDG). Hodnota DHCSR pred pripojenim se cte z logu noveho openocd (-d3): examine ji cte
jako prvni vec a teprve pak C_DEBUGEN nastavi sam (Target.dhcsr_on_attach)."""

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
    dbg.stop_all()  # cesta atexit
    d = target.dhcsr_on_attach()
    assert not d & C_DEBUGEN, f"C_DEBUGEN still set after stop_all(): DHCSR 0x{d:08x}"
    assert not d & S_HALT


def test_halted_core_is_resumed_and_released(target):
    """Zastavene jadro (jako po bkpt) se pri uvolneni rozbehne a C_DEBUGEN se vynuluje."""
    target.tcl("halt")
    assert target.core_state()["halted"]
    target.stop()
    d = target.dhcsr_on_attach()
    assert not d & (C_DEBUGEN | S_HALT), f"DHCSR 0x{d:08x}"
