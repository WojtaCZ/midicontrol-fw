"""Base se pripojuje a bonduje jen s ovladacem (Review Focus 5) a automat prikazoveho rezimu neztraci prikazy.

Cizi zarizeni: PC (BlueZ) se k base umi pripojit, jen kdyz base advertisuje (normalne ne, viz report) a BlueZ spojeni
sam hned ukonci, takze %STREAM_OPEN% nikdy neprijde. Proto se stavove zpravy modulu vkladaji pres sondu do
Bluetooth::_statusQueue (dbg.inject_status) - firmware na ne reaguje stejne jako na skutecne; odpovedi base jdou
skutecne do RN4870 a jsou videt na D3."""

import time

import pytest

import dbg
import la
from conftest import remote_alive, wait_until

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
