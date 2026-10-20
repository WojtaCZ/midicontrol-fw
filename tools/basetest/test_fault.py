"""Zaznamy o chybach (fault::Record v no-init RAM), smyckovy watchdog (SysTick, 1000 ms) a IWDG (~2 s) base.

Chyby se vyvolavaji zapisem do `fault::testInject` (firmware ho cte jednou za pruchod hlavni smyckou):
1 = cteni z 0xFFFFFFF0 (BusFault / HardFault), 2 = nekonecna smycka s povolenymi prerusenimi (loop stall),
3 = nekonecna smycka se zakazanymi prerusenimi (IWDG). Pro cestu "zaznam + reset" musi byt DHCSR.C_DEBUGEN = 0:
`base.release_debug()` (README "Fault tests"). Kazdy test zacina i konci s bezici base, USB a otevrenym logem."""

import random
import re
import subprocess
import time

import pytest

import dbg
import loglink
import usblink
from conftest import remote_alive, wait_until
from test_log import BANNER_RE, PRE_END, PRE_START, keepalive_rtt, log, wait_log_port  # noqa: F401 - fixture log

INJ_BUS, INJ_STALL, INJ_IWDG = 1, 2, 3
FAULT_RE = r"E fault: (HARDFAULT|MEMMANAGE|BUSFAULT|USAGEFAULT|ERROR|LOOP_STALL) #(\d+)"


def reboot(link, log, action, gone_within: float, back_within: float = 6.0):
    """`action()` ma base resetovat. Ceka, az log CDC zmizi (do `gone_within` s od zacatku akce) a znovu se objevi.
    Vraci (novy LogLink, cas do zmizeni USB [s])."""
    log.close()
    link.detach()
    t0 = time.monotonic()
    action()
    while True:
        try:
            loglink.find_log_port()
        except RuntimeError:
            break
        assert time.monotonic() - t0 < gone_within, f"base did not reset within {gone_within} s"
        time.sleep(0.02)
    t_gone = time.monotonic() - t0
    port = wait_log_port(back_within)
    for _ in range(30):
        try:
            new = loglink.LogLink(port)
            break
        except Exception:  # noqa: BLE001 - port jeste neni otevritelny
            time.sleep(0.1)
    else:
        raise AssertionError("log CDC not openable")
    link.attach(settle=0.0)
    return new, t_gone


def boot(log, timeout=5.0):
    """(radky logu pred resetem, radky noveho bootu od banneru do 1,5 s po nem)."""
    first = log.lines(timeout, pred=lambda l: l == PRE_START or re.search(BANNER_RE, l))
    before = []
    if first and first[-1] == PRE_START:
        before = log.lines(timeout, pred=lambda l: l == PRE_END)
        assert before and before[-1] == PRE_END, before
        first = log.lines(timeout, pred=lambda l: re.search(BANNER_RE, l))
    assert first and re.search(BANNER_RE, first[-1]), first
    return before, [first[-1]] + log.lines(1.5)


def inject(base, value: int, release=True):
    """Zapis fault::testInject za behu jadra; s release i odpojeni ladeni (C_DEBUGEN = 0) a konec openocd."""
    addr = base.sym("fault::testInject")
    if release:
        # Nejdriv C_DEBUGEN = 0, teprve pak injekce: chyba muze nastat hned v dalsim pruchodu smyckou
        base.tcl(f"write_memory 0xE000EDF0 32 {{0xA05F0000}}; write_memory 0x{addr:08x} 8 {{{value}}}")
        base.stop()
    else:
        base.write(addr, bytes([value]))


def field(lines, name: str) -> int:
    for l in lines:
        m = re.search(rf"\b{name}=0x([0-9a-f]{{8}})", l)
        if m:
            return int(m.group(1), 16)
    raise AssertionError(f"{name}= not in {lines}")


def fault_lines(lines):
    return [l for l in lines if " fault: " in l]


@pytest.fixture(autouse=True)
def base_running(base):
    """Pred i po testu: jadro bezi (nikdy nenechat zastavene)."""
    st = base.core_state()
    if st["halted"]:
        base.resume()
    yield
    st = base.core_state()
    if st["halted"]:
        base.reset()


def test_hardfault_record(link, log, base):
    log, t_gone = reboot(link, log, lambda: inject(base, INJ_BUS), gone_within=3.0)
    try:
        before, lines = boot(log)
        f = fault_lines(lines)
        m = next((re.search(FAULT_RE, l) for l in f if re.search(FAULT_RE, l)), None)
        assert m and m.group(1) in ("HARDFAULT", "BUSFAULT"), lines
        pc = field(f, "pc")
        fn = base.addr2line(pc)
        assert fn.startswith("main"), (hex(pc), fn)
        cfsr_line = next(l for l in f if "cfsr=" in l)
        assert re.search(r"PRECISERR|IBUSERR|BFARVALID", cfsr_line), cfsr_line
        assert field(f, "bfar") == 0xFFFFFFF0, f
        # Log pred resetem: posledni radky pred chybou (zavreni portu pri reboot()) a radek z cesty chyby
        assert any("I usb: log port closed" in l for l in before), before[-10:]
        assert any(re.search(r"E fault: (HARDFAULT|BUSFAULT) ", l) for l in before), before[-10:]
        assert any(re.search(r"reset: .*SOFTWARE", l) for l in lines), lines
        # dump: stejny zaznam jako odpovedi na prikaz
        rep = log.command("dump")
        assert rep and re.search(r"^(HARDFAULT|BUSFAULT) #\d+", rep[0]), rep
        assert any(f"pc=0x{pc:08x}" in l for l in rep), rep
        rec = base.fault_record()
        assert rec and rec["pc"] == pc and dbg.FAULT_KINDS[rec["kind"]] == m.group(1)
    finally:
        log.close()


def test_loop_stall_record(link, log, base):
    log, t_gone = reboot(link, log, lambda: inject(base, INJ_STALL), gone_within=3.0)
    try:
        assert 0.8 < t_gone < 2.0, f"loop stall reset after {t_gone:.2f} s (expected ~1 s)"
        before, lines = boot(log)
        f = fault_lines(lines)
        assert any(re.search(r"E fault: LOOP_STALL #\d+", l) for l in f), lines
        pc = field(f, "pc")
        fn = base.addr2line(pc)
        assert fn.startswith("fault::testSpin"), (hex(pc), fn)
        assert any(re.search(r"reset: .*SOFTWARE", l) for l in lines), lines
        assert not any("IWDG" in l for l in lines if "reset:" in l), lines
    finally:
        log.close()


def test_iwdg_reset(link, log, base):
    log, t_gone = reboot(link, log, lambda: inject(base, INJ_IWDG), gone_within=4.0)
    try:
        assert 1.5 < t_gone < 3.0, f"IWDG reset after {t_gone:.2f} s (expected ~2 s)"
        before, lines = boot(log)
        assert any(re.search(r"reset: .*IWDG", l) for l in lines), lines
        # Se zakazanymi prerusenimi SysTick nebezi: zadny novy zaznam (E), jen pripadne starsi uz ohlaseny (I)
        assert not [l for l in lines if " E fault: " in l], lines
    finally:
        log.close()


def test_no_bogus_fault_after_garbage(link, log, base):
    rnd = random.Random(0x6C)
    data = bytes(rnd.randrange(256) for _ in range(dbg.NOINIT_SIZE))

    def garbage_and_reset():
        base.tcl("halt")
        try:
            for off in range(0, dbg.NOINIT_SIZE, 256):
                chunk = data[off:off + 256]
                base.tcl(f"write_memory 0x{dbg.NOINIT_ADDR + off:08x} 8 {{{' '.join(f'0x{b:02x}' for b in chunk)}}}")
        finally:
            base.tcl("reset run")

    log, _ = reboot(link, log, garbage_and_reset, gone_within=3.0)
    try:
        before, lines = boot(log)
        assert before == [], before[:5]  # hlavicka ringu neplatna -> log zacina znovu
        assert not fault_lines(lines), lines
        assert base.fault_record() is None
        assert log.command("dump") == ["no fault record"]
        with log._cv:
            assert not [l for _, l in log.history if l == PRE_START]
    finally:
        log.close()


def test_debugger_halts_not_resets(link, log, base):
    """Sonda pripojena (C_DEBUGEN = 1): chyba -> zaznam + bkpt, jadro stoji (zadny reset). Behem zastaveni
    neobsluhuje USB, proto je link i log odpojeny po celou dobu zastaveni (jinak zapis keepalive vyprsi)."""
    st = base.core_state()
    assert st["dhcsr"] & 1, "C_DEBUGEN not set with openocd attached"
    seen = {}

    def halt_check_reset():
        try:
            inject(base, INJ_BUS, release=False)
            wait_until(lambda: base.core_state()["halted"], 2.0, what="core halted at bkpt")
            seen["pc"] = int(re.search(r"0x[0-9a-f]+", base.tcl("capture {halt}; capture {reg pc}")).group(0), 16)
            seen["rec"] = base.fault_record()
        finally:
            base.reset()

    log, _ = reboot(link, log, halt_check_reset, gone_within=5.0)
    try:
        assert base.addr2line(seen["pc"]).startswith("fault::finish"), (hex(seen["pc"]), base.addr2line(seen["pc"]))
        rec = seen["rec"]
        assert rec and dbg.FAULT_KINDS[rec["kind"]] in ("HARDFAULT", "BUSFAULT"), rec
        assert rec["bfar"] == 0xFFFFFFF0
        assert base.addr2line(rec["pc"]).startswith("main")
        _, lines = boot(log)
        assert any(re.search(FAULT_RE, l) for l in lines), lines
    finally:
        log.close()


def _base_midi_port() -> str:
    out = subprocess.run(["amidi", "-l"], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines()[1:]:
        m = re.match(r"^\S+\s+(hw:\d+,\d+,\d+)\s+(.*?)\s*$", line)
        if m and "MIDIControl" in m.group(2):
            return m.group(1)
    raise AssertionError(f"base USB-MIDI port not found:\n{out}")


def _uptime_ms(line: str) -> int:
    m = re.match(r"^\[ *(\d+)\.(\d{3})\]", line)
    return int(m.group(1)) * 1000 + int(m.group(2)) if m else -1


def test_long_operations_do_not_trip(link, log, base):
    """BLE rekonfigurace ($$$ ... R,1), cely OLED flush a 200 DIN not: zadny reset (USB zustava, uptime roste)."""
    was_alive = remote_alive(base)
    t0 = base.u("stmcpp::systick::ticks_")
    w0 = time.monotonic()
    log.skip()
    # Hned po bootu muze fronta prikazu RN4870 jeste drzet konfiguraci z bootu (configure vraci 1): zkouset
    end = time.monotonic() + 15.0
    while base.call("Bluetooth::configure()") != 0:
        assert time.monotonic() < end, "Bluetooth::configure() not accepted within 15 s"
        time.sleep(1.0)
    if base.has_function("GUI::renderForce"):
        base.call("GUI::renderForce()")
    base.call("Oled::flush(500)")
    notes = " ".join(f"90 {40 + i % 40:02X} 64 80 {40 + i % 40:02X} 00" for i in range(200))
    subprocess.run(["amidi", "-p", _base_midi_port(), "-S", notes], check=True, timeout=10)
    end = time.monotonic() + 6.0
    while time.monotonic() < end:
        keepalive_rtt(link)
        time.sleep(0.2)
    lines = log.lines(0.5)
    assert not [l for l in lines if re.search(BANNER_RE, l) or "reset:" in l or l == PRE_START], lines
    assert any("ble" in l for l in lines), lines  # R,1 -> modul se restartoval
    ups = [_uptime_ms(l) for l in lines if loglink.is_log_line(l)]
    assert ups == sorted(ups), ups
    elapsed = base.u("stmcpp::systick::ticks_") - t0
    assert elapsed >= (time.monotonic() - w0 - 1.0) * 1000, (elapsed, time.monotonic() - w0)
    usblink.find_port()  # protokolove CDC stale tu
    if was_alive:
        wait_until(lambda: remote_alive(base), 30.0, interval=0.5, what="remote reconnected after R,1")
