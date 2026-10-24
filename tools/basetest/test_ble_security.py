"""Base se pripojuje a bonduje jen s ovladacem (Review Focus 5), provadi ramce jen od zabezpeceneho ovladace
(final review I1) a automat prikazoveho rezimu neztraci prikazy.

Cizi zarizeni: PC (BlueZ) se k base umi pripojit, jen kdyz base advertisuje (normalne ne, viz report) a BlueZ spojeni
sam hned ukonci, takze %STREAM_OPEN% nikdy neprijde. Proto se stavove zpravy modulu vkladaji pres sondu do
Bluetooth::_statusQueue (dbg.inject_status) - firmware na ne reaguje stejne jako na skutecne; odpovedi base jdou
skutecne do RN4870 a jsou videt na D3."""

import time

import pytest

import dbg
import la
import proto
from conftest import remote_alive, wait_until
from proto import ADDR_BASE, ADDR_REMOTE, MT

STRANGER = "10B1DF04F934"  # adresa BT adapteru PC
REMOTE = "D88039FE5996"
BAUD = 115200


def tx_text(sr) -> bytes:
    return bytes(b for b in la.uart(sr, la.CH["base_ble_tx"], BAUD) if 32 <= b < 127 or b == 13)


@pytest.fixture
def remote_away(base, remote_probe):
    """RN4870 remote v resetu, base odpojena; na konci uvolnit a pockat na zabezpecene opetovne spojeni."""
    remote_probe.hold_ble_reset(True)
    try:
        wait_until(lambda: not base.read_bool("Bluetooth::_connected"), 10, what="base disconnected")
        yield base
    finally:
        remote_probe.hold_ble_reset(False)
    wait_until(lambda: base.read_bool("Bluetooth::_streamOpen"), 15, what="base reconnected to the remote")
    wait_until(lambda: base.read_bool("Bluetooth::_bonded"), 3, what="reconnected link secured")
    wait_until(lambda: remote_alive(base), 6, interval=0.5, what="remote alive")


def test_inbound_stranger_dropped_not_secured(remote_away):
    base = remote_away
    proc, sr = la.start(7, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        base.inject_status(f"CONNECT,0,{STRANGER}")
        time.sleep(0.3)
        base.inject_status("STREAM_OPEN")
        time.sleep(2.0)
        bonded = base.read_bool("Bluetooth::_bonded")
    finally:
        la.finish(proc, sr, 30)
        base.inject_status("DISCONNECT")  # stav firmware zpet (modul ve skutecnosti pripojen nebyl)
    tx = tx_text(sr)
    assert b"K,1\r" in tx, f"stranger not disconnected: {tx!r}"
    assert b"B\r" not in tx, f"stranger got B (silent bond): {tx!r}"
    assert not bonded


RX_RING_SIZE = 512  # Bluetooth::_rxRing: RingBuffer<uint8_t, 512> = 512 B dat, _head (u32), _tail (u32)


def inject_ble_data(base, data: bytes):
    """Base: vlozi bajty do Bluetooth::_rxRing, jako by prisly z RN4870 v datovem rezimu (rozdeli je process())."""
    addr = base.sym("Bluetooth::_rxRing")
    with base.safe_halt():
        head = int.from_bytes(base.read(addr + RX_RING_SIZE, 4), "little")
        first = data[: RX_RING_SIZE - head]
        base.write(addr + head, first)
        if len(first) < len(data):
            base.write(addr, data[len(first):])
        head = (head + len(data)) % RX_RING_SIZE
        base.write(addr + RX_RING_SIZE, head.to_bytes(4, "little"))


def test_frames_need_secured_remote(remote_away):
    """Podvrzena adresa ovladace: %CONNECT% s adresou ovladace + %STREAM_OPEN%, ale bez %SECURED% -> SET_CURRENT
    z BLE se neprovede (PA4 beze zmeny), %ERR_SEC% -> K,1. Kladna kontrola: po %SECURED% na spojeni se spravnou
    adresou se stejne vlozeny ramec provede (cesta vkladani funguje)."""
    base = remote_away
    ssr0 = base.gpio("A", 4)
    proc, sr = la.start(9, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    accepted_before = accepted_after = None
    ssr_unsecured = ssr_secured = None
    try:
        base.inject_status(f"CONNECT,0,{REMOTE}")
        time.sleep(0.3)
        base.inject_status("STREAM_OPEN")
        time.sleep(1.0)  # B jde do modulu (ve skutecnosti nepripojen), zadny %SECURED%
        inject_ble_data(base, proto.encode(ADDR_REMOTE, ADDR_BASE, 0x61, MT.SET_CURRENT, bytes([1 - ssr0])))
        time.sleep(0.5)
        ssr_unsecured = base.gpio("A", 4)
        accepted_before = base.read_bool("Bluetooth::_peerAccepted")
        base.inject_status("ERR_SEC")
        time.sleep(1.5)
        # Kladna kontrola na novem spojeni
        base.inject_status(f"CONNECT,0,{REMOTE}")
        time.sleep(0.3)
        base.inject_status("STREAM_OPEN")
        time.sleep(1.0)
        base.inject_status("SECURED")
        time.sleep(0.3)
        accepted_after = base.read_bool("Bluetooth::_peerAccepted")
        inject_ble_data(base, proto.encode(ADDR_REMOTE, ADDR_BASE, 0x62, MT.SET_CURRENT, bytes([1 - ssr0])))
        time.sleep(0.5)
        ssr_secured = base.gpio("A", 4)
    finally:
        la.finish(proc, sr, 30)
        if base.gpio("A", 4) != ssr0 and base.read_bool("Bluetooth::_peerAccepted"):
            inject_ble_data(base, proto.encode(ADDR_REMOTE, ADDR_BASE, 0x63, MT.SET_CURRENT, bytes([ssr0])))
            time.sleep(0.5)
        base.inject_status("DISCONNECT")  # stav firmware zpet (modul ve skutecnosti pripojen nebyl)
    tx = tx_text(sr)
    assert ssr_unsecured == ssr0, "SET_CURRENT from an unsecured link was executed"
    assert accepted_before is False, "peer accepted without %SECURED%"
    assert b"K,1\r" in tx, f"%ERR_SEC% outside pairing did not disconnect: {tx!r}"
    assert accepted_after is True, "secured link to the stored remote not accepted"
    assert ssr_secured == 1 - ssr0, "frame from the secured remote not executed (injection path broken?)"
    assert base.gpio("A", 4) == ssr0, "SSR not restored"


def test_connect_while_scanning_kicked(remote_away):
    """%CONNECT% behem skenovani parovani (nikdo se k nam pripojovat nema) -> K,1, sken bezi dal."""
    base = remote_away
    proc, sr = la.start(8, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        assert base.call("Bluetooth::startPairing()") == 0
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == dbg.PAIR_SCANNING, 2, what="SCANNING")
        time.sleep(0.5)
        base.inject_status(f"CONNECT,0,{STRANGER}")
        time.sleep(1.5)
        state = base.read_u8("Bluetooth::_pairingState")
        accepted = base.read_bool("Bluetooth::_peerAccepted")
    finally:
        la.finish(proc, sr, 30)
        base.inject_status("DISCONNECT")
        # pairing doběhne timeoutem (15 s), aby fixture mohla obnovit spojení s ovladačem
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") not in (1, 2, 3), 20, interval=0.5, what="pairing ended")
    tx = tx_text(sr)
    after_f = tx[tx.index(b"F\r"):]
    assert b"K,1\r" in after_f, f"connect during SCANNING not kicked: {tx!r}"
    assert state == dbg.PAIR_SCANNING, f"pairing state {state}"
    assert not accepted


SETTINGS_REMOTE_ADDR_OFFSET = 8  # settings::ramCopy.remoteAddr (za 8 B dat verze 1, src/settings.cpp)


def test_no_reconnect_without_stored_remote(remote_away):
    """Prazdna adresa ovladace (upgrade z nastaveni v1, ruling R32 b): base se k nicemu nepripojuje (zadne Z/C kazdych
    5 s). Adresa se smaze jen v RAM kopii nastaveni (flash beze zmeny) a na konci se vrati. Kladna kontrola: po
    vraceni adresy prijde "C" do ~6 s. Napoveda "Sparuj ovladac" v titulku hlavniho menu se kontroluje okem."""
    base = remote_away
    addr = base.sym("settings::ramCopy") + SETTINGS_REMOTE_ADDR_OFFSET
    saved = base.read(addr, 13)
    assert saved[:12].decode(errors="replace") == REMOTE, f"stored remote {saved!r}"
    proc, sr = la.start(20, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)  # zachyt bezi ~1 s pred smazanim
    try:
        base.write(addr, b"\0")
        time.sleep(12.0)
        base.write(addr, saved)
        time.sleep(7.0)
    finally:
        base.write(addr, saved)
        la.finish(proc, sr, 40)
    data, _ = la.uart_timed(sr, la.CH["base_ble_tx"], BAUD, 1e6)

    def text(t0, t1):
        return bytes(b for t, b in data if t0 <= t < t1)

    # 2.5 s: rezerva na start zachytu a pokus o pripojeni rozbehnuty tesne pred smazanim
    assert b"C\r" not in text(2.5, 13.0), f"reconnect attempt while no remote was stored: {text(2.5, 13.0)!r}"
    assert b"C\r" in text(13.0, 21.0), f"no reconnect after the address was restored: {text(13.0, 21.0)!r}"


def test_pairing_ignores_foreign_connect(remote_away):
    base = remote_away
    proc, sr = la.start(8, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        assert base.call("Bluetooth::startPairing()") == 0
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == dbg.PAIR_SCANNING, 2, what="SCANNING")
        time.sleep(0.5)
        base.inject_status(f"{REMOTE},0,MIDIControlRemote,,F5")  # reklama remote -> X, C,0,<remote>
        wait_until(lambda: base.read_u8("Bluetooth::_pairingState") == dbg.PAIR_CONNECTING, 2, what="CONNECTING")
        time.sleep(0.5)
        base.inject_status(f"CONNECT,0,{STRANGER}")  # pripoji se nekdo jiny
        time.sleep(1.5)
        state = base.read_u8("Bluetooth::_pairingState")
    finally:
        la.finish(proc, sr, 30)
        base.inject_status("DISCONNECT")
    tx = tx_text(sr)
    assert f"C,0,{REMOTE}\r".encode() in tx, tx
    after_connect = tx[tx.index(f"C,0,{REMOTE}\r".encode()):]
    assert b"B\r" not in after_connect, f"foreign %CONNECT% treated as the remote: {tx!r}"
    assert b"K,1\r" in after_connect, tx
    assert state == dbg.PAIR_FAILED, f"pairing state {state}"


def test_command_queued_during_exit_survives(base):
    """Prikaz pridany do fronty, kdyz automat ceka na END po '---' (EXIT_CMD), se musi odeslat.
    Stav se nastavi pres sondu v bezpecnem miste smycky: uloha "V" dobehla (_jobIndex = _jobCount = 1, EXIT_CMD,
    END jeste neprisel) a za ni je pridany "D" (_jobCount = 2) - presne to, co udela queueCommand() v tu chvili.
    Casove razitko automatu je stare, takze EXIT_TIMEOUT vyprsi hned po pokracovani."""
    jobs, count, index = base.sym("Bluetooth::_jobCommands"), base.sym("Bluetooth::_jobCount"), base.sym("Bluetooth::_jobIndex")
    state, saw_end = base.sym("Bluetooth::_engineState"), base.sym("Bluetooth::_sawEnd")
    proc, sr = la.start(5, channels="D2,D3", samplerate="1M")
    time.sleep(1.0)
    try:
        with base.safe_halt():
            assert base.read(state, 1) == b"\0" and base.read(count, 1) == b"\0", "engine busy"
            base.write(jobs, b"V\0")
            base.write(jobs + 32, b"D\0")
            base.write(index, b"\x01")
            base.write(count, b"\x02")
            base.write(saw_end, b"\0")
            base.write(state, b"\x05")  # EngineState::EXIT_CMD
        time.sleep(2.0)
        left = base.read(count, 1)[0], base.read(state, 1)[0]
    finally:
        la.finish(proc, sr, 30)
    tx = tx_text(sr)
    assert b"D\r" in tx, f"command queued during EXIT_CMD was dropped: {tx!r}"
    assert left == (0, 0), f"engine not idle afterwards: count/state {left}"
