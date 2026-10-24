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
OP_ERASE, OP_PROGRAM, OP_MEASURE_ERASE, OP_MEASURE_PROGRAM = range(4)

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


def glitch_bin() -> str:
    """Prelozi glitch.c (Thumb-1, bezi na M0+ i M4) do glitch/build/glitch.bin, je-li starsi nez zdroj."""
    src = os.path.join(GLITCH_DIR, "glitch.c")
    out_dir = os.path.join(GLITCH_DIR, "build")
    elf, binf = os.path.join(out_dir, "glitch.elf"), os.path.join(out_dir, "glitch.bin")
    if not os.path.exists(binf) or os.path.getmtime(binf) < os.path.getmtime(src):
        os.makedirs(out_dir, exist_ok=True)
        subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-ffreestanding", "-nostdlib",
                        "-T", os.path.join(GLITCH_DIR, "glitch.ld"), src, "-o", elf], check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", elf, binf], check=True)
    return binf


class Board:
    def __init__(self, name: str):
        self.name = name
        self.b = BOARDS[name]
        self.t = dbg.Target(name)
        self._syms = None

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
    def tcl(self, cmd: str) -> str:
        return self.t.tcl(cmd)

    def halted(self) -> bool:
        return self.t.core_state()["halted"]

    def halt(self):
        self.tcl("halt 500")

    def reg(self, name: str) -> int:
        out = self.tcl(f"reg {name}")
        return int(re.search(r"0x[0-9a-fA-F]+", out.split(":")[-1]).group(0), 16)

    def reset(self):
        """Reset sondou (SYSRESETREQ, reset_config none); jadro bezi dal."""
        self.t.reset()

    def stop(self):
        self.t.stop()

    def u32(self, addr: int) -> int:
        return int(self.tcl(f"read_memory 0x{addr:08x} 32 1").split()[0], 0)

    def flash_read(self, addr: int, n: int) -> bytes:
        words = self.tcl(f"read_memory 0x{addr:08x} 32 {(n + 3) // 4}").split()
        return b"".join(struct.pack("<I", int(w, 0)) for w in words)[:n]

    def eccr(self) -> int:
        return self.u32(FLASH_ECCR)

    def where(self) -> dict:
        """Kde jadro je: kind = app | bootloader | hang (NMI/HardFault/lockup v bootloaderu nebo jinde) | unknown.
        Kratce zastavi jadro (pc, xPSR) a pusti ho dal."""
        st = self.t.core_state()
        was_halted = st["halted"]
        if not was_halted:
            self.halt()
        pc, xpsr = self.reg("pc"), self.reg("xPSR")
        if not was_halted:
            self.tcl("resume")
        ipsr = xpsr & 0x1FF
        if st["lockup"] or ipsr in (2, 3):
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
        if watch_write:
            self.tcl(f"wp 0x{addr:08x} 4 w")
        else:
            self.tcl(f"bp 0x{addr:08x} 2 hw")

    def disarm_all(self):
        self.tcl("capture {rbp all}; capture {rwp all}")

    def wait_halt(self, timeout: float) -> bool:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.halted():
                return True
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
                    self.tcl("resume")
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
        self.tcl("resume")
        if op >= OP_MEASURE_ERASE:
            if not self.wait_halt(timeout):
                raise RuntimeError("glitch measure did not finish")
            r = self.u32(PARAMS_ADDR + 28)
            self.tcl("reset run")
            time.sleep(0.2)
            return r
        time.sleep(0.3)  # reset uz probehl
        if self.halted():
            self.tcl("resume")
        return None

    # -- obnova
    def swd_restore(self, hexfile=None):
        """Nahraje -update.hex (cela oblast aplikace, 0xFF az do APP_END) a spusti; posledni zachrana."""
        hexfile = hexfile or self.b["update_hex"]
        self.halt()
        out = self.tcl(f"flash write_image erase {{{hexfile}}}; verify_image {{{hexfile}}}")
        self.tcl("reset run")
        time.sleep(0.3)
        if self.halted():
            self.tcl("resume")
        return out

    def app_matches(self, update_bin=None) -> bool:
        """Oblast aplikace ve flash = update soubor (vcetne traileru)."""
        img = open(update_bin or self.b["update_bin"], "rb").read()
        return self.flash_read(self.b["app"], len(img)) == img
