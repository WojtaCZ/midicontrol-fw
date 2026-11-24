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
    try:  # the RSSI poll ($$$ ... M ... ---) every 7 s would break tests that halt at an idle engine or capture the exact UART text
        t.write(t.sym("Bluetooth::_rssiPollDisabled"), b"\x01")
    except Exception as e:  # older image: no flag (a base reset clears it; test_link_stats turns the poll on itself)
        print(f"rssiPollDisabled not set: {e}")
    yield t
    t.stop()


@pytest.fixture(scope="session")
def remote():
    import dbg
    t = dbg.Target("remote")
    try:  # the remote switches itself off after 20 min without a key press; a long suite must not be cut short
        t.write(t.sym("base::Power::autoOffDisabled"), b"\x01")
        t.write(t.sym("Bluetooth::rssiPollDisabled"), b"\x01")  # the remote's RSSI session (11 s) would disturb command/data mode tests
    except Exception as e:  # older remote image without the flag, or no probe: tests that need the remote skip/fail on their own
        print(f"autoOffDisabled not set: {e}")
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


def forget_remote(base):
    """The base forgets all bonds (U,Z) and disconnects if connected. Order U,Z -> K,1: an auto-reconnect ("C" to the
    last bonded device) right after the disconnect would succeed if the bond still existed."""
    base.call('Bluetooth::queueCommand("U,Z")')
    if base.read_bool("Bluetooth::_connected"):
        base.call('Bluetooth::queueCommand("K,1")')
    wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 10, what="base disconnected")
    # The base still has settings::remoteAddr and keeps reconnecting ("Z" + CE every 5 s, "cmd failed: Z"); a reconnect that
    # runs into startPairing() made its $$$ time out (bench 2026-11-24). Stop it until the next link change.
    base.write(base.sym("_reconnectNeeded"), b"\x00")
    time.sleep(1)


def _prompt(t) -> bytes:
    return t.read(t.sym("GUI::pairingPrompt"), 32).split(b"\0")[0]


def pair_confirmed(base, remote, timeout=60, open_window=True) -> int:
    """D3 (2026-11-24): a pairing as the user does it. Remote "Sparovat zakl." (pairRequest opens the 60 s window), base
    "Sparovat ovladac" (startPairing); after the link secures both show "Sparovat? Kod nnnnnn" (cursor on Ne) and Ano is
    pressed on both: remote Left+OK through GUI::keypress, base through the confirm screen's result callback (the base
    Release ELF has no GUI::keypress). Asserts both show the same code. Returns Bluetooth::_pairingState of the base."""
    import dbg
    forget_remote(base)
    if open_window:
        remote.write(remote.sym("Bluetooth::pairRequest"), b"\x01")
        time.sleep(0.5)
    base.call("Bluetooth::startPairing()")
    confirm = base.sym("GUI::pairingConfirmScreen")
    done = lambda: base.read_u8("Bluetooth::_pairingState") in (dbg.PAIR_DONE, dbg.PAIR_FAILED)
    # both sessions in CONFIRM (s_pair byte 1, pairing::State::CONFIRM = 2), not only the screens: a reset session (double
    # %BONDED%/%SECURED% on the remote, bench 2026-11-24) still showed the screen
    in_confirm = lambda t: t.read(t.sym("s_pair"), 2)[1] == 2
    shown = lambda: (base.current_screen() == confirm and remote.screen_name() == "pairingScreen"
                     and in_confirm(base) and in_confirm(remote))
    wait_until(lambda: done() or shown(), timeout, what="pairing code shown on both devices")
    if not done():
        assert _prompt(base) == _prompt(remote), f"codes differ: base {_prompt(base)!r}, remote {_prompt(remote)!r}"
        assert _prompt(base).startswith(b"Sparovat? Kod "), _prompt(base)
        remote.key(dbg.MF, dbg.ENTER)  # MF = InputEvent::Left: Ne -> Ano, then OK
        base.call("GUI::onPairConfirmResult(1, 0)")
        wait_until(done, 15, what="pairing finished after Ano on both")
    return base.read_u8("Bluetooth::_pairingState")
