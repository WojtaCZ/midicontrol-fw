"""Gaps 6/7/8 (SYS-PAIR-04, SYS-PAIR-06, SYS-FW-07, SYS-LINK-05): the remote gives a link that carries frames only to its
bonded base, a new base is paired through an explicit window on the remote, and RN status text a peer writes into the data
stream (F2 "%CONNECT%" / F4 "CMD>") changes nothing. The logic is peerguard::Guard (midicontroller-fw src/logic/peer_guard,
host tests test_peer_guard.cpp); these tests check the firmware around it on the bench.

Needs both probes and a remote that is paired with the bench base (test_pairing_window_stores_the_base pairs it when it
is not). BASETEST_BASE_MAC = the bench base's module MAC (default 408432009A19, public address)."""

import os
import subprocess
import time

import pytest

from conftest import remote_alive, wait_until

pytestmark = pytest.mark.hw

BASE_MAC = os.environ.get("BASETEST_BASE_MAC", "408432009A19").upper()
STRANGER = "10B1DF04F934"
LINK_S = 30.0  # the base retries C every few seconds; reconnect + B + first keepalive take ~8 s

# peerguard::Guard layout (src/logic/peer_guard.hpp, arm-none-eabi, 48 bytes): checked against nm -S below
G_BOND, G_TYPE, G_WINDOW, G_WSTART, G_LINKED, G_ACCEPTED, G_SECURED, G_REFUSE_SENT = 0, 26, 28, 32, 36, 37, 39, 40
GUARD_SIZE = 48


def _nm(remote):
    return subprocess.run(["arm-none-eabi-nm", "-C", "-S", remote.elf], capture_output=True, text=True).stdout


def _anon(remote, name):
    """Address (and size) of Bluetooth::(anonymous namespace)::<name>, from the symbol table (the Release ELF has no DWARF)."""
    for line in _nm(remote).splitlines():
        if line.endswith(f"Bluetooth::(anonymous namespace)::{name}"):
            parts = line.split()
            return int(parts[0], 16), int(parts[1], 16)
    raise KeyError(name)


@pytest.fixture(scope="module")
def guard(remote_probe):
    addr, size = _anon(remote_probe, "guard")
    assert size == GUARD_SIZE, f"peerguard::Guard is {size} B, the test expects {GUARD_SIZE}: update the offsets"
    return addr


def state(remote, guard):
    g = remote.read(guard, GUARD_SIZE)
    refusals = int.from_bytes(remote.read(_anon(remote, "refusals")[0], 2), "little")
    return dict(bond=g[G_BOND:G_BOND + 12].split(b"\0")[0].decode(errors="replace"), type=chr(g[G_TYPE]), window=g[G_WINDOW],
                linked=g[G_LINKED], accepted=g[G_ACCEPTED], secured=g[G_SECURED], refusals=refusals)


def stored_bond(remote) -> str:
    """settings::ramCopy.baseMac: what main saved (and wrote to the settings page) after a pairing."""
    raw = remote.read(remote.sym("settings::ramCopy"), 24)
    return raw[8:21].split(b"\0")[0].decode(errors="replace")


def inject_rx(remote, data: bytes):
    """Puts bytes into the remote's USART2 RX ring as if the module had sent them (the RX ISR does not run while halted)."""
    buf, size = _anon(remote, "rxBuf")
    head_addr = _anon(remote, "rxHead")[0]
    with remote.safe_halt():
        head = int.from_bytes(remote.read(head_addr, 2), "little")
        for b in data:
            remote.write(buf + head, bytes([b]))
            head = (head + 1) % size
        remote.write(head_addr, head.to_bytes(2, "little"))


def linked_to_base(base, remote, guard) -> bool:
    s = state(remote, guard)
    return s["linked"] and s["accepted"] and s["secured"] and remote_alive(base)


@pytest.fixture
def paired(base, remote_probe, guard):
    """The remote is bonded to the bench base and the link is up (pairs it through the window when not)."""
    remote = remote_probe
    if state(remote, guard)["bond"] != BASE_MAC:
        remote.write(remote.sym("Bluetooth::pairRequest"), b"\x01")
        wait_until(lambda: state(remote, guard)["bond"] == BASE_MAC, 70.0, what="pairing window stores the bench base")
    wait_until(lambda: linked_to_base(base, remote, guard), LINK_S, what="remote linked to the bonded base")
    return remote


def test_bond_is_the_bench_base(paired, guard):
    s = state(paired, guard)
    assert s["bond"] == BASE_MAC and s["type"] == "0"
    assert stored_bond(paired) == BASE_MAC
    assert paired.cstr("Bluetooth::(anonymous namespace)::peerMac", 13) == BASE_MAC


def test_cmd_prompt_from_peer_is_ignored(base, paired, guard):
    """F4: "CMD>" in the data stream used to switch the firmware to command mode; it then sent no frames until a reboot."""
    mode = paired.sym("Bluetooth::(anonymous namespace)::_mode")
    assert paired.read(mode, 1)[0] == 1  # Mode::DATA
    inject_rx(paired, b"CMD> ")
    time.sleep(0.2)
    assert paired.read(mode, 1)[0] == 1, "a CMD> the module did not answer to our $$$ switched to command mode"
    time.sleep(8.0)  # > KEEPALIVE_LOSS: the base would drop a remote that stopped answering
    assert linked_to_base(base, paired, guard)


def test_repeat_connect_from_peer_is_ignored(base, paired, guard):
    """F2: a second %CONNECT% while the link is up is text the peer wrote; the peer and the link stay as they are."""
    before = state(paired, guard)
    inject_rx(paired, f"%CONNECT,0,{STRANGER},0041%".encode())
    time.sleep(0.3)
    after = state(paired, guard)
    assert after["refusals"] == before["refusals"]
    assert paired.cstr("Bluetooth::(anonymous namespace)::peerMac", 13) == BASE_MAC
    time.sleep(7.0)
    assert linked_to_base(base, paired, guard)


def test_unknown_status_from_peer_is_ignored(base, paired, guard):
    """Unknown status text a peer writes ("%FOO,1%") changes nothing."""
    inject_rx(paired, b"%FOO,1%")
    time.sleep(0.3)
    assert linked_to_base(base, paired, guard)


def test_other_central_is_refused(base, paired, guard):
    """Gap 6: a central that is not the bond gets K,1 and never a frame. The bond is changed in RAM only (the settings page
    keeps the base), so the module's accept list still lets the base in and the firmware guard is what refuses it."""
    remote = paired
    bogus = b"000000000001"
    before = state(remote, guard)["refusals"]
    remote.write(guard + G_BOND, bogus)
    try:
        remote.call("Bluetooth::refusePeer()")  # K,1: end the current link; the base reconnects with C at once
        wait_until(lambda: state(remote, guard)["refusals"] > before, LINK_S, what="reconnecting base refused")
        time.sleep(1.0)
        s = state(remote, guard)
        assert not (s["accepted"] and s["secured"]), s
        wait_until(lambda: not remote_alive(base), 10.0, what="base sees no remote")
    finally:
        remote.write(guard + G_BOND, BASE_MAC.encode())
    assert stored_bond(remote) == BASE_MAC
    wait_until(lambda: linked_to_base(base, remote, guard), 60.0, what="bonded base accepted again")


def test_pairing_window_stores_the_base(base, paired, guard):
    """Gap 7: Nastaveni -> "Sparovat zakl." opens a 60 s window (pairRequest = the menu item), drops the current link and
    clears the accept list; the first central that connects and secures becomes the bond and is saved."""
    remote = paired
    remote.write(guard + G_BOND, b"000000000002")  # proves the window really stores the new base
    remote.write(remote.sym("Bluetooth::pairRequest"), b"\x01")
    wait_until(lambda: state(remote, guard)["window"] or state(remote, guard)["bond"] == BASE_MAC, 5.0, what="window open")
    wait_until(lambda: state(remote, guard)["bond"] == BASE_MAC, 70.0, what="window stores the bench base")
    s = state(remote, guard)
    assert not s["window"], "one base per window"
    assert stored_bond(remote) == BASE_MAC
    wait_until(lambda: linked_to_base(base, remote, guard), LINK_S, what="link after pairing")


def test_pairing_window_closes_without_a_base(base, paired, guard):
    """A window that ends with no new base keeps the old bond and closes by itself. The window start is moved back at once
    instead of waiting 60 s, so the window is over before the base (dropped by openPairing) reconnects."""
    remote = paired
    remote.write(remote.sym("Bluetooth::pairRequest"), b"\x01")
    wait_until(lambda: state(remote, guard)["window"], 5.0, interval=0.05, what="window open")
    start = int.from_bytes(remote.read(guard + G_WSTART, 4), "little")
    remote.write(guard + G_WSTART, ((start - 61000) & 0xFFFFFFFF).to_bytes(4, "little"))
    wait_until(lambda: not state(remote, guard)["window"], 2.0, interval=0.05, what="expired window closed")
    assert state(remote, guard)["bond"] == BASE_MAC
    assert stored_bond(remote) == BASE_MAC
    wait_until(lambda: linked_to_base(base, remote, guard), LINK_S, what="bonded base back after the window closed")


def test_own_disconnect_lets_the_base_back_in(base, paired, guard):
    """The RNBD350 answers our own K,1 with AOK and sends no %DISCONNECT% (bench 2026-11-12). The firmware must end the link
    itself, otherwise the guard keeps the old link up and ignores the base's reconnect as F2 text: linked, but no frames."""
    remote = paired
    before = state(remote, guard)["refusals"]
    remote.call("Bluetooth::disconnect()")
    wait_until(lambda: not remote_alive(base), 10.0, what="K,1 ended the link")
    wait_until(lambda: linked_to_base(base, remote, guard), LINK_S, what="reconnected base accepted and secured")
    assert state(remote, guard)["refusals"] == before
