"""Fault injection into the base and remote bootloaders (tests test_boot_*.py, plan docs/superpowers/plans/2026-10-24-...).

A power loss is imitated on the bench in three ways (from coarse to precise):
  * a probe reset at a given moment of the transfer (`Board.reset()`),
  * an HW breakpoint / DWT watchpoint at a given bootloader step and then a reset (`break_then_reset`): FPB and DWT belong to the
    debug domain, a system reset does not clear them, so the bp can be set in the running application and is caught only in the bootloader,
  * a glitch routine in SRAM (`glitch/glitch.c`): starts a real flash erase/program and after N iterations SYSRESETREQ,
    i.e. interrupts the flash operation itself.
The bench cannot do a real power disconnect (the base is also powered outside the USB port, see plan Task 0)."""

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
    """A proven product defect (not a harness one): only this exception is accepted by the xfail(raises=ProductDefect) tests F4/F4b/F5,
    any other error in them is a normal failure."""


def glitch_bin() -> str:
    """Compiles glitch.c (Thumb-1, runs on M0+ and M4) into glitch/build/glitch.bin if it is older than the source."""
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
    """Probe reads during a reset / during copying by the bootloader sometimes fail ("read_memory: failed to read memory"). After a reset of the
    remote, the running openocd (hla) may not be able to read at all, while a new one can (bench 2026-10-24): after 1 s of errors openocd is restarted."""
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
        self._armed = []  # [(addr, watch_write)]: a new openocd clears FPB/DWT on examine, restart_server restores them

    def restart_server(self):
        """openocd again (without releasing debug: just terminate the process), the new one reattaches to the core."""
        p = dbg._servers.pop(self.name, None)
        if p and p.poll() is None:
            p.terminate()
            p.wait(5)
        self.t.start()
        for addr, watch in self._armed:
            self._arm_hw(addr, watch)

    # -- bootloader symbols
    def bl_sym(self, name: str) -> int:
        """Address of a bootloader function by its demangled name without parameters (e.g. 'boot::flashprog::erasePageAt')."""
        if self._syms is None:
            out = subprocess.run(["arm-none-eabi-nm", "-C", self.b["bl_elf"]], capture_output=True, text=True, check=True).stdout
            self._syms = {}
            for line in out.splitlines():
                parts = line.split(" ", 2)
                if len(parts) == 3 and parts[1] in "tTwW":
                    self._syms.setdefault(parts[2].split("(")[0], int(parts[0], 16))
        return self._syms[name]

    # -- core
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
        out = self.tcl(f"reg {name} force")  # always from the core, not from the openocd cache
        m = re.search(r"0x[0-9a-fA-F]+", out)
        if not m:
            raise RuntimeError(f"reg {name}: {out!r}")
        return int(m.group(0), 16)

    def reset(self):
        """Probe reset (SYSRESETREQ, reset_config none); the core keeps running."""
        self.t.reset()

    def stop(self):
        try:
            if dbg.probe_holder(self.name) is None and dbg._servers.get(self.name):
                self.disarm_all()
        except Exception:  # noqa: BLE001 - core/probe unavailable
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
        """Where the core is: kind = app | bootloader | hang (fault exception or lockup anywhere) | unknown.
        Briefly halts the core (pc, xPSR) and lets it run on."""
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
        # NMI, HardFault, MemManage, BusFault, UsageFault: an infinite loop in the bootloader, in the application with a probe bkpt
        # (without a probe a reset and the same again) -> always "hang" for the tests
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

    # -- breakpoints / watchpoints
    def arm(self, addr: int, watch_write=False):
        """HW bp / DWT write watchpoint. A wp set while the core runs (hla) silently has no effect: it is set while the
        core is halted (bench 2026-10-24)."""
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
        """Removes all bp/wp, directly in FPB/DWT as well: those survive a system reset and the end of openocd (only POR clears them) and
        openocd 0.12 has no `rwp all`. A forgotten FPB bp without debug (C_DEBUGEN = 0) = HardFault (bench 2026-10-24)."""
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
        """Waits for a halt (bp/wp). While the internal flash is being erased the bus stalls and DHCSR reads fail for ~1 s; connection
        recovery (hla) then clears FPB/DWT (bench 2026-10-24: FP_COMP0 = 0 after errors). Hence: survive the errors without
        restarting openocd and set the bp/wp again after the first successful read. BOOTFI_TRACE=<file> logs the progress."""
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
        """Waits for the `hits`-th hit of the bp/wp at `addr` (sets it unless `armed`), then removes the bp/wp and resets the core
        (= a power loss exactly here). False if the bp was not hit within `timeout` (the bp is removed, the core runs)."""
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
        """Runs the glitch routine from a halted core. op 0/1: flash operation + reset after `delay` iterations (returns None);
        op 2/3: measurement, returns the number of iterations until the operation ends (bit 31 = error, SR in the low bits)."""
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
        time.sleep(0.3)  # the reset has already happened
        if self.halted():
            self.t.resume()
        return None

    # -- recovery
    def swd_restore(self, hexfile=None):
        """Flashes -update.hex (the whole application region, 0xFF up to APP_END) and runs it; the last resort."""
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
        """The application region in flash = the update file (including the trailer); `image` = path or bytes, default the build."""
        img = image if isinstance(image, (bytes, bytearray)) else open(image or self.b["update_bin"], "rb").read()
        return self.flash_read(self.b["app"], len(img)) == img
