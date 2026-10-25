"""Vkladani poruch do bootloaderu base a remote (testy test_boot_*.py, plan docs/superpowers/plans/2026-10-24-...).

Vypadek napajeni se na lavici napodobi tremi zpusoby (od hrubeho k presnemu):
  * reset sondou v danem okamziku prenosu (`Board.reset()`),
  * HW breakpoint / DWT watchpoint na danem kroku bootloaderu a pak reset (`break_then_reset`): FPB a DWT patri do
    ladici domeny, systemovy reset je nemaze, takze lze bp nastavit v bezici aplikaci a zachyti se az v bootloaderu,
  * glitch rutina v SRAM (`glitch/glitch.c`): spusti skutecne mazani/programovani flash a po N iteracich SYSRESETREQ,
    tj. prerusi samotnou flash operaci.
Skutecne odpojeni napajeni lavice neumi (base je napajena i mimo USB port, viz plan Task 0)."""

import os
import re
import struct
import subprocess
import time

import dbg

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.abspath(os.path.join(HERE, "..", ".."))
REMOTE_FW = os.path.abspath(os.path.join(FW, "..", "midicontroller-fw"))
GLITCH_DIR = os.path.join(HERE, "glitch")
GLITCH_ADDR, PARAMS_ADDR = 0x20001000, 0x20000F00
FLASH, FLASH_ECCR, FLASH_SR, FLASH_CR = 0x40022000, 0x40022018, 0x40022010, 0x40022014
ECCD, ECCC = 1 << 31, 1 << 30
OP_ERASE, OP_PROGRAM, OP_MEASURE_ERASE, OP_MEASURE_PROGRAM, OP_ERASE_THEN_PROGRAM, OP_MEASURE_ERASE_THEN_PROGRAM = range(6)

BOARDS = {
    "base": dict(cpu="stm32g4x.cpu", app=0x08008000, app_end=0x0801F800, page=2048,
                 bl_elf=os.path.join(FW, "build", "bootloader", "base-bootloader.elf"),
                 update_hex=os.path.join(FW, "build", "midicontrol-fw-update.hex"),
                 update_bin=os.path.join(FW, "build", "midicontrol-fw-update.bin")),
    "remote": dict(cpu="stm32g0x.cpu", app=0x08004000, app_end=0x0801F800, page=2048,
                   bl_elf=os.path.join(REMOTE_FW, "build", "bootloader", "remote-bootloader.elf"),
                   update_hex=os.path.join(REMOTE_FW, "build", "midicontroller-fw-update.hex"),
                   update_bin=os.path.join(REMOTE_FW, "build", "midicontroller-fw-update.bin")),
}


class ProductDefect(AssertionError):
    """Prokazana vada produktu (ne harnessu): jen tuto vyjimku prijimaji xfail(raises=ProductDefect) testy F4/F4b/F5,
    jakakoli jina chyba v nich je normalni selhani."""


def glitch_bin() -> str:
    """Prelozi glitch.c (Thumb-1, bezi na M0+ i M4) do glitch/build/glitch.bin, je-li starsi nez zdroj."""
    src = os.path.join(GLITCH_DIR, "glitch.c")
    out_dir = os.path.join(GLITCH_DIR, "build")
    elf, binf = os.path.join(out_dir, "glitch.elf"), os.path.join(out_dir, "glitch.bin")
    ld = os.path.join(GLITCH_DIR, "glitch.ld")
    if not os.path.exists(binf) or os.path.getmtime(binf) < max(os.path.getmtime(src), os.path.getmtime(ld)):
        os.makedirs(out_dir, exist_ok=True)
        subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-ffreestanding", "-nostdlib",
                        "-T", os.path.join(GLITCH_DIR, "glitch.ld"), src, "-o", elf], check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", elf, binf], check=True)
    return binf


def _retry(fn, timeout=8.0, restart=None):
    """Probe cteni behem resetu / kopirovani bootloaderem obcas selze ("read_memory: failed to read memory"). Po resetu
    remote bezici openocd (hla) uz cist nemusi vubec, novy ano (bench 2026-10-24): po 1 s chyb se openocd restartuje."""
    end = time.monotonic() + timeout
    first_fail = None
    while True:
        try:
            return fn()
        except (ValueError, IndexError, RuntimeError, OSError):
            now = time.monotonic()
            if now > end:
                raise
            first_fail = first_fail or now
            if restart and now - first_fail > 1.0:
                restart()
                first_fail = None
            time.sleep(0.3)


class Board:
    def __init__(self, name: str):
        self.name = name
        self.b = BOARDS[name]
        self.t = dbg.Target(name)
        self._syms = None
        self._armed = []  # [(addr, watch_write)]: novy openocd pri examine FPB/DWT vymaze, restart_server je obnovi

    def restart_server(self):
        """openocd znovu (bez uvolneni ladeni: jen ukoncit proces), novy se k jadru pripoji znovu."""
        p = dbg._servers.pop(self.name, None)
        if p and p.poll() is None:
            p.terminate()
            p.wait(5)
        self.t.start()
        for addr, watch in self._armed:
            self._arm_hw(addr, watch)

    # -- symboly bootloaderu
    def bl_sym(self, name: str) -> int:
        """Adresa funkce bootloaderu podle demanglovaneho jmena bez parametru (napr. 'boot::flashprog::erasePageAt')."""
        if self._syms is None:
            out = subprocess.run(["arm-none-eabi-nm", "-C", self.b["bl_elf"]], capture_output=True, text=True, check=True).stdout
            self._syms = {}
            for line in out.splitlines():
                parts = line.split(" ", 2)
                if len(parts) == 3 and parts[1] in "tTwW":
                    self._syms.setdefault(parts[2].split("(")[0], int(parts[0], 16))
        return self._syms[name]

    # -- jadro
    def tcl(self, cmd: str, timeout: float = 5.0) -> str:
        if timeout <= 5.0:
            return self.t.tcl(cmd)
        import socket
        self.t.start()
        with socket.create_connection(("127.0.0.1", self.t.c["tcl"]), timeout) as s:
            s.sendall(cmd.encode() + b"\x1a")
            buf = b""
            while not buf.endswith(b"\x1a"):
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
        return buf[:-1].decode(errors="replace")

    def core_state(self) -> dict:
        def rd():
            out = self.tcl("read_memory 0xE000EDF0 32 1")
            try:
                d = int(out.split()[0], 0)
            except (ValueError, IndexError):
                raise RuntimeError(f"DHCSR read failed: {out.strip()[:300]!r}") from None
            return {"halted": bool(d & (1 << 17)), "sleeping": bool(d & (1 << 18)), "lockup": bool(d & (1 << 19)), "dhcsr": d}
        return _retry(rd, restart=self.restart_server)

    def halted(self) -> bool:
        return self.core_state()["halted"]

    def halt(self):
        self.tcl("halt 500")

    def reg(self, name: str) -> int:
        out = self.tcl(f"reg {name} force")  # vzdy z jadra, ne z cache openocd
        m = re.search(r"0x[0-9a-fA-F]+", out)
        if not m:
            raise RuntimeError(f"reg {name}: {out!r}")
        return int(m.group(0), 16)

    def reset(self):
        """Reset sondou (SYSRESETREQ, reset_config none); jadro bezi dal."""
        self.t.reset()

    def stop(self):
        try:
            if dbg.probe_holder(self.name) is None and dbg._servers.get(self.name):
                self.disarm_all()
        except Exception:  # noqa: BLE001 - jadro/sonda nedostupne
            pass
        self.t.stop()

    def u32(self, addr: int) -> int:
        return _retry(lambda: int(self.tcl(f"read_memory 0x{addr:08x} 32 1").split()[0], 0), restart=self.restart_server)

    def flash_read(self, addr: int, n: int) -> bytes:
        def rd():
            words = self.tcl(f"read_memory 0x{addr:08x} 32 {(n + 3) // 4}").split()
            out = b"".join(struct.pack("<I", int(w, 0)) for w in words)[:n]
            if len(out) != n:
                raise ValueError(f"short read {len(out)}/{n}")
            return out
        return _retry(rd, restart=self.restart_server)

    def eccr(self) -> int:
        return self.u32(FLASH_ECCR)

    def where(self) -> dict:
        """Kde jadro je: kind = app | bootloader | hang (fault exception nebo lockup kdekoli) | unknown.
        Kratce zastavi jadro (pc, xPSR) a pusti ho dal."""
        def snap():
            st = self.core_state()
            was_halted = st["halted"]
            if not was_halted:
                self.halt()
            try:
                return st, self.reg("pc"), self.reg("xPSR")
            finally:
                if not was_halted:
                    self.t.resume()
        st, pc, xpsr = _retry(snap)
        ipsr = xpsr & 0x1FF
        # NMI, HardFault, MemManage, BusFault, UsageFault: v bootloaderu nekonecna smycka, v aplikaci s probem bkpt
        # (bez proby reset a znovu totez) -> pro testy vzdy "hang"
        if st["lockup"] or ipsr in (2, 3, 4, 5, 6):
            kind = "hang"
        elif 0x08000000 <= pc < self.b["app"]:
            kind = "bootloader"
        elif self.b["app"] <= pc < self.b["app_end"]:
            kind = "app"
        else:
            kind = "unknown"
        if kind == "bootloader" and pc in (self.bl_sym("Default_Handler"),):
            kind = "hang"
        return dict(kind=kind, pc=pc, ipsr=ipsr, lockup=st["lockup"])

    # -- breakpointy / watchpointy
    def arm(self, addr: int, watch_write=False):
        """HW bp / DWT write watchpoint. wp nastaveny za behu jadra (hla) se tise neprojevi: nastavuje se pri zastavenem
        jadru (bench 2026-10-24)."""
        self._arm_hw(addr, watch_write)
        self._armed.append((addr, watch_write))

    def _arm_hw(self, addr: int, watch_write: bool):
        was_halted = self.halted()
        if not was_halted:
            self.halt()
        self.tcl(f"capture {{rwp 0x{addr:08x}}}" if watch_write else f"capture {{rbp 0x{addr:08x}}}")
        out = self.tcl(f"wp 0x{addr:08x} 4 w" if watch_write else f"bp 0x{addr:08x} 2 hw")
        if not was_halted:
            self.t.resume()
        if "rror" in out:
            raise RuntimeError(f"arm 0x{addr:08x}: {out}")

    def disarm_all(self):
        """Zrusi vsechny bp/wp i primo v FPB/DWT: ty prezivaji systemovy reset i konec openocd (jen POR je nuluje) a
        openocd 0.12 nema `rwp all`. Zapomenuty FPB bp bez ladeni (C_DEBUGEN = 0) = HardFault (bench 2026-10-24)."""
        self._armed = []
        self.tcl("capture {rbp all}")
        for line in self.tcl("wp").splitlines():
            m = re.match(r"address: (0x[0-9a-f]+)", line.strip())
            if m:
                self.tcl(f"capture {{rwp {m.group(1)}}}")
        n_comp = (self.u32(0xE0002000) >> 4) & 0xF  # FP_CTRL.NUM_CODE
        for i in range(n_comp):
            self.tcl(f"write_memory 0x{0xE0002008 + 4 * i:08x} 32 {{0}}")
        n_dwt = self.u32(0xE0001000) >> 28  # DWT_CTRL.NUMCOMP
        for i in range(n_dwt):
            self.tcl(f"write_memory 0x{0xE0001028 + 16 * i:08x} 32 {{0}}")

    def wait_halt(self, timeout: float) -> bool:
        """Ceka na zastaveni (bp/wp). Behem mazani interni flash sbernice stoji a cteni DHCSR ~1 s selhava; obnova
        spojeni (hla) pritom FPB/DWT vymaze (bench 2026-10-24: FP_COMP0 = 0 po chybach). Proto: chyby prezit bez
        restartu openocd a po prvnim uspesnem cteni nastavit bp/wp znovu. BOOTFI_TRACE=<soubor> zapisuje prubeh."""
        end = time.monotonic() + timeout
        t0 = time.monotonic()
        trace = os.environ.get("BOOTFI_TRACE")
        failed = False
        while time.monotonic() < end:
            out = self.tcl("read_memory 0xE000EDF0 32 1")
            if trace:
                with open(trace, "a") as f:
                    f.write(f"{time.monotonic() - t0:6.2f} {out.strip()[:60]}\n")
            try:
                dhcsr = int(out.split()[0], 0)
            except (ValueError, IndexError):
                failed = True
                time.sleep(0.05)
                continue
            if dhcsr & (1 << 17):
                return True
            if failed:
                failed = False
                for addr, watch in self._armed:
                    self._arm_hw(addr, watch)
                if trace:
                    with open(trace, "a") as f:
                        f.write(f"{time.monotonic() - t0:6.2f} re-armed {[hex(a) for a, _ in self._armed]}\n")
            time.sleep(0.02)
        return False

    def break_then_reset(self, addr: int, *, hits=1, watch_write=False, timeout=30.0, armed=False, before_reset=None) -> bool:
        """Ceka na `hits`-ty zasah bp/wp na `addr` (nastavi ho, neni-li `armed`), pak zrusi bp/wp a resetuje jadro
        (= vypadek napajeni presne tady). False, kdyz se bp do `timeout` nezasahl (bp se zrusi, jadro bezi)."""
        if not armed:
            self.arm(addr, watch_write)
        try:
            for i in range(hits):
                if not self.wait_halt(timeout):
                    return False
                if i + 1 < hits:
                    self.t.resume()
                    time.sleep(0.005)
            if before_reset:
                before_reset()
            return True
        finally:
            self.disarm_all()
            if self.halted():
                self.tcl("reset run")
                time.sleep(0.2)

    # -- glitch
    def glitch(self, op: int, addr: int, delay: int = 0, w0=0, w1=0, timeout=5.0):
        """Spusti glitch rutinu ze zastaveneho jadra. op 0/1: flash operace + reset po `delay` iteracich (vraci None);
        op 2/3: mereni, vraci pocet iteraci do konce operace (bit 31 = chyba, SR v dolnich bitech)."""
        page = (addr - 0x08000000) // self.b["page"]
        self.halt()
        self.tcl(f"load_image {glitch_bin()} 0x{GLITCH_ADDR:08x} bin")
        words = [FLASH, op, addr, delay, w0, w1, page, 0xFFFFFFFF]
        self.tcl(f"write_memory 0x{PARAMS_ADDR:08x} 32 {{{' '.join(f'0x{w:08x}' for w in words)}}}")
        self.tcl(f"reg pc 0x{GLITCH_ADDR:08x}")
        self.tcl("reg xPSR 0x01000000")
        self.tcl("reg primask 1")
        self.t.resume()
        if op in (OP_MEASURE_ERASE, OP_MEASURE_PROGRAM, OP_MEASURE_ERASE_THEN_PROGRAM):
            if not self.wait_halt(timeout):
                raise RuntimeError("glitch measure did not finish")
            r = self.u32(PARAMS_ADDR + 28)
            self.tcl("reset run")
            time.sleep(0.2)
            return r
        time.sleep(0.3)  # reset uz probehl
        if self.halted():
            self.t.resume()
        return None

    # -- obnova
    def swd_restore(self, hexfile=None):
        """Nahraje -update.hex (cela oblast aplikace, 0xFF az do APP_END) a spusti; posledni zachrana."""
        hexfile = hexfile or self.b["update_hex"]
        self.halt()
        for attempt in range(2):
            out = self.tcl(f"flash write_image erase {{{hexfile}}}; verify_image {{{hexfile}}}", timeout=180)
            self.tcl("reset run")
            time.sleep(1.5)
            if self.halted():
                self.t.resume()
            if "verified" in out and self.where()["kind"] == "app":
                break
            self.halt()
        return out

    def app_matches(self, image=None) -> bool:
        """Oblast aplikace ve flash = update soubor (vcetne traileru); `image` = cesta nebo bajty, default build."""
        img = image if isinstance(image, (bytes, bytearray)) else open(image or self.b["update_bin"], "rb").read()
        return self.flash_read(self.b["app"], len(img)) == img
