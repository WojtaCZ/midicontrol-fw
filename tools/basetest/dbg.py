"""ST-Link helpers for both the base and the remote: openocd (TCL, reads without halting) + gdb (symbols, function calls).
Model: ../midicontroller-fw/tools/bletest/dbg.py. One probe = one openocd; two probes run at the same time."""

import atexit
import contextlib
import functools
import os
import re
import socket
import subprocess
import time

FW = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REMOTE_FW = os.path.abspath(os.path.join(FW, "..", "midicontroller-fw"))
GDB = "arm-none-eabi-gdb"
NM = "arm-none-eabi-nm"

CONFIGS = {
    "base": dict(serial="39FF71064B43353943122243", cfg=os.path.join(FW, "openocd.cfg"), elf=os.path.join(FW, "build", "midicontrol-fw.elf"),
                 gdb=3334, tcl=6668, telnet=6667, cwd=FW, gpio=0x48000000, safe_bp="Bluetooth::process"),
    "remote": dict(serial="37FF70064E59313840161343", cfg=os.path.join(REMOTE_FW, "openocd.cfg"),
                   # The remote Release ELF has no DWARF; BASETEST_REMOTE_ELF = a -g copy with the same code (README)
                   elf=os.environ.get("BASETEST_REMOTE_ELF", os.path.join(REMOTE_FW, "build", "midicontroller-fw.elf")), gdb=3333, tcl=6666, telnet=6665, cwd=REMOTE_FW, gpio=0x50000000,
                   safe_bp="base::Keyboard::process"),
}

# fault::Record (base, inc/fault.hpp): 8 B header + 18 x u32 + rev[8] + crc
FAULT_RECORD_SIZE = 92
FAULT_KINDS = {1: "HARDFAULT", 2: "MEMMANAGE", 3: "BUSFAULT", 4: "USAGEFAULT", 5: "ERROR", 6: "LOOP_STALL"}
NOINIT_ADDR, NOINIT_SIZE = 0x20007800, 2048

# ui::InputEvent (remote)
UP, DOWN, MF, RIGHT, ENTER, BACK = 0, 1, 2, 3, 4, 5
DIGIT = {str(d): 6 + d for d in range(10)}
# PlayerScreen::State (remote)
IDLE, STARTING, PLAYING, STOPPING = 0, 1, 2, 3
# Bluetooth::PairingState (base)
PAIR_IDLE, PAIR_SCANNING, PAIR_CONNECTING, PAIR_BONDING, PAIR_DONE, PAIR_FAILED = range(6)
# Remote RN4870 RST = PB8 (active low); GPIOB BSRR on the G071
REMOTE_GPIOB_BSRR = 0x50000400 + 0x18
REMOTE_BLE_RST_PIN = 8
# Base RN4870 RST = PA1 (active low); GPIOA BSRR on the G431
BASE_GPIOA_BSRR = 0x48000000 + 0x18
BASE_BLE_RST_PIN = 1

def probe_holder(name: str):
    """A foreign process (openocd of another session) that holds the probe `name`; None = free or ours."""
    ours = _servers.get(name)
    r = subprocess.run(["pgrep", "-af", f"adapter serial {CONFIGS[name]['serial']}"], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        pid = int(line.split()[0])
        if ours and ours.pid == pid:
            continue
        if "openocd" in line and "pgrep" not in line:
            return line
    return None

_servers = {}


DHCSR = 0xE000EDF0
DHCSR_RELEASE = 0xA05F0000  # DBGKEY, C_DEBUGEN = 0, C_HALT = 0


def _raw_tcl(port: int, cmd: str) -> str:
    with socket.create_connection(("127.0.0.1", port), 5) as s:
        s.sendall(cmd.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    return buf[:-1].decode(errors="replace")


def _release_debugen(name: str):
    """Before terminating openocd: DHCSR.C_DEBUGEN = 0 (the core keeps running). Otherwise a real fault on the bench would end up in
    fault::finish() at a bkpt (and hang forever with DBG_IWDG_STOP). A halted core is resumed first."""
    p = _servers.get(name)
    if not p or p.poll() is not None:
        return
    port = CONFIGS[name]["tcl"]
    try:
        dhcsr = int(_raw_tcl(port, f"read_memory 0x{DHCSR:08x} 32 1").split()[0], 0)
        if dhcsr & (1 << 17):
            _raw_tcl(port, "capture {halt}; capture {resume}")
        _raw_tcl(port, f"write_memory 0x{DHCSR:08x} 32 {{0x{DHCSR_RELEASE:08x}}}")
    except (OSError, ValueError, IndexError):
        pass


def stop_all():
    for name in list(_servers):
        _release_debugen(name)
    for p in _servers.values():
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
    _servers.clear()


atexit.register(stop_all)


class Target:
    def __init__(self, name: str):
        self.name = name
        self.c = CONFIGS[name]
        self.elf = self.c["elf"]
        self._sym = {}

    # -- openocd
    def start(self, _retry=True):
        p = _servers.get(self.name)
        if p and p.poll() is None:
            return
        c = self.c
        _servers[self.name] = subprocess.Popen(
            ["openocd", "-c", f"adapter serial {c['serial']}", "-f", c["cfg"],
             "-c", f"gdb_port {c['gdb']}; tcl_port {c['tcl']}; telnet_port {c['telnet']}"],
            cwd=c["cwd"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(300):  # the first connection to the probe can take up to 10 s
            if _servers[self.name].poll() is not None:
                if _retry:  # sometimes the probe is not ready yet after the previous connection
                    time.sleep(1)
                    return self.start(_retry=False)
                raise RuntimeError(f"openocd ({self.name}) exited - another instance on this probe?")
            try:
                socket.create_connection(("127.0.0.1", c["tcl"]), 0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError(f"openocd ({self.name}) did not start")

    def stop(self):
        _release_debugen(self.name)
        p = _servers.pop(self.name, None)
        if p and p.poll() is None:
            p.terminate()
            p.wait(5)

    def tcl(self, cmd: str) -> str:
        self.start()
        return _raw_tcl(self.c["tcl"], cmd)

    def dhcsr_on_attach(self) -> int:
        """DHCSR as the core had it BEFORE attaching: a new openocd (-d3) reads DHCSR as the first thing during examine
        and then sets C_DEBUGEN itself. Here it is logged, the value is parsed out and debug is released again (the core runs).
        This session's server is terminated first (stop() = release)."""
        self.stop()
        c = self.c
        r = subprocess.run(["openocd", "-d3", "-c", f"adapter serial {c['serial']}", "-f", c["cfg"],
                            "-c", f"gdb_port {c['gdb']}; tcl_port {c['tcl']}; telnet_port {c['telnet']}",
                            "-c", "init", "-c", f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}", "-c", "shutdown"],
                           cwd=c["cwd"], capture_output=True, text=True, timeout=60)
        m = re.search(r"target_read_u32\(\): address: 0xe000edf0, value: (0x[0-9a-f]+)", r.stdout + r.stderr)
        if not m:
            raise RuntimeError(f"DHCSR not seen in openocd log ({self.name}): {(r.stdout + r.stderr)[-300:]}")
        return int(m.group(1), 16)

    def read(self, addr: int, n: int) -> bytes:
        """Memory read without halting the core (AHB-AP)."""
        out = self.tcl(f"read_memory 0x{addr:08x} 8 {n}")
        return bytes(int(x, 0) for x in out.split())

    def write(self, addr: int, data: bytes):
        """Memory write without halting the core (AHB-AP), e.g. for a volatile flag."""
        self.tcl(f"write_memory 0x{addr:08x} 8 {{{' '.join(f'0x{b:02x}' for b in data)}}}")

    def core_state(self) -> dict:
        dhcsr = int(self.tcl("read_memory 0xE000EDF0 32 1").split()[0], 0)
        return {"halted": bool(dhcsr & (1 << 17)), "sleeping": bool(dhcsr & (1 << 18)), "lockup": bool(dhcsr & (1 << 19)), "dhcsr": dhcsr}

    def resume(self):
        # a bare "resume" is ignored by openocd when it does not know about the halt (e.g. a new openocd) -> halt first
        self.tcl("capture {halt}; capture {resume}")

    def pc_if_halted(self):
        return self.tcl("reg pc") if self.core_state()["halted"] else None

    # -- symbols (from the ELF only)
    def _static(self, expr: str) -> str:
        out = subprocess.run([GDB, "-q", "-nx", "-batch", "-ex", f"print {expr}", self.elf], capture_output=True, text=True).stdout
        m = re.search(r"^\$1 = (.*)$", out, re.M)
        if not m:
            raise KeyError(f"{expr}: {out}")
        return m.group(1)

    def sym(self, name: str) -> int:
        """Address of a symbol, e.g. 'Debug::_logRing'. First the symbol table (nm -C, works without DWARF too),
        otherwise gdb (struct fields, e.g. 'GUI::playerScreen.state_', needs -g)."""
        if name not in self._sym:
            nm = subprocess.run([NM, "-C", self.elf], capture_output=True, text=True).stdout
            for line in nm.splitlines():
                parts = line.split(" ", 2)
                if len(parts) == 3 and parts[2] == name:
                    self._sym[name] = int(parts[0], 16)
                    break
            else:
                addr = self._lto_priv_sym(name)
                if addr is not None:
                    self._sym[name] = addr
                    return addr
                q = f"'{name}'" if "(anonymous namespace)" in name and not name.startswith("'") else name
                self._sym[name] = int(re.search(r"0x[0-9a-f]+", self._static(f"&{q}")).group(0), 16)
        return self._sym[name]

    def _lto_priv_sym(self, name: str):
        """Static variables that LTO renames to <mangled>.lto_priv.N (nm -C then does not demangle them; it depends on the
        LTO partitioning - a change elsewhere in the firmware can trigger it). The demangled base of the name is searched."""
        nm = subprocess.run([NM, self.elf], capture_output=True, text=True).stdout
        cands = []
        for line in nm.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3:
                m = re.fullmatch(r"(_Z\S+?)\.lto_priv\.\d+", parts[2])
                if m:
                    cands.append((int(parts[0], 16), m.group(1)))
        if not cands:
            return None
        dem = subprocess.run([NM.replace("nm", "c++filt")], input="\n".join(c[1] for c in cands), capture_output=True, text=True).stdout.splitlines()
        for (addr, _), d in zip(cands, dem):
            if d == name:
                return addr
        return None

    def u(self, name: str, size: int = 4) -> int:
        return self.read_uint(self.sym(name), size)

    def read_uint(self, addr: int, size: int = 4) -> int:
        """Little-endian number without halting the core. Aligned 2/4 B with a single 16/32-bit access: a byte-wise read (read())
        gets torn across the transfer when an ISR is incrementing the value at that moment (soak 2026-10-24: "systick went back
        271871 -> 271819", ticks_ = 0x425FF). Everything else (1 B, unaligned, other lengths) byte by byte."""
        if size in (2, 4) and addr % size == 0:
            return int(self.tcl(f"read_memory 0x{addr:08x} {size * 8} 1").split()[0], 0)
        return int.from_bytes(self.read(addr, size), "little")

    def read_u8(self, name: str) -> int:
        return self.u(name, 1)

    def read_bool(self, name: str) -> bool:
        return bool(self.u(name, 1))

    def write32(self, addr: int, value: int):
        self.tcl(f"write_memory 0x{addr:08x} 32 {{0x{value:08x}}}")

    def cstr(self, name: str, n: int = 64) -> str:
        return self.read(self.sym(name), n).split(b"\0")[0].decode(errors="replace")

    # -- GPIO
    def gpio(self, port: str, pin: int) -> int:
        """Pin state from IDR (port 'A'..): G431 GPIO at 0x48000000, G071 at 0x50000000, stride 0x400."""
        n = ord(port.upper()) - ord("A")
        base = self.c["gpio"] + 0x400 * n
        idr = int.from_bytes(self.read(base + 0x10, 4), "little")
        return (idr >> pin) & 1

    # -- gdb
    def _gdb(self, cmds, timeout=20):
        self.start()
        args = [GDB, "-q", "-nx", "-batch", "-ex", f"target extended-remote :{self.c['gdb']}", "-ex", "set confirm off"]
        for c in cmds:
            args += ["-ex", c]
        # never leave the core halted: resume before detach
        args += ["-ex", "monitor resume", "-ex", "detach"]
        try:
            r = subprocess.run(args + [self.elf], capture_output=True, text=True, timeout=timeout)
        finally:
            if self.core_state()["halted"]:
                self.resume()
        return r

    def reset(self):
        """Core reset via the probe (reset run); the core is never left halted."""
        self.tcl("reset run")
        time.sleep(0.2)
        if self.core_state()["halted"]:
            self.resume()

    def release_debug(self):
        """Detach debug so that the firmware sees DHCSR.C_DEBUGEN = 0 (fault -> record + NVIC_SystemReset instead of bkpt).
        C_DEBUGEN is cleared only by POR or a DHCSR write; the ST-Link (hla) sets it on attach and a system reset leaves it.
        Hence: DHCSR = DBGKEY without C_DEBUGEN (0xA05F0000, the core keeps running), then terminate openocd right away (the next tcl() starts
        it again and sets C_DEBUGEN again). See the README "Fault tests"."""
        self.tcl("write_memory 0xE000EDF0 32 {0xA05F0000}")
        self.stop()

    def addr2line(self, addr: int) -> str:
        """Function name for an address (arm-none-eabi-addr2line -f -C); a Release ELF without DWARF -> from the symbol table."""
        r = subprocess.run([NM.replace("nm", "addr2line"), "-f", "-C", "-e", self.elf, f"0x{addr:08x}"], capture_output=True, text=True)
        return r.stdout.splitlines()[0] if r.stdout else "??"

    def fault_record(self):
        """Base: fault::_record in .noinit as a dict, or None (bad magic/CRC). Layout = fault::Record (fault.hpp)."""
        import struct
        import zlib
        raw = self.read(self.sym("fault::_record"), FAULT_RECORD_SIZE)
        magic, kind, count, code = struct.unpack_from("<IBBH", raw, 0)
        regs = struct.unpack_from("<18I", raw, 8)
        rev = raw[80:88].split(b"\0")[0].decode(errors="replace")
        crc = struct.unpack_from("<I", raw, 88)[0]
        if magic != 0x46415554 or crc != zlib.crc32(raw[:88]) & 0xFFFFFFFF:
            return None
        names = ["r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr", "exc_return", "msp", "psp", "ipsr", "cfsr", "hfsr",
                 "mmfar", "bfar", "afsr", "uptime_ms"]
        return dict(kind=kind, count=count, code=code, rev=rev, **dict(zip(names, regs)))

    def function_symbol(self, name: str) -> str:
        """Full demangled function name from the symbol table, e.g. 'Bluetooth::queueCommand(char const*)'."""
        nm = subprocess.run([NM, "-C", self.elf], capture_output=True, text=True).stdout
        for line in nm.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[1] in "tTwW" and parts[2].startswith(name + "(") and "[clone" not in parts[2]:
                return parts[2]
        raise KeyError(f"function {name} not in the symbol table of {self.elf} (inlined/removed?)")

    def has_function(self, name: str) -> bool:
        try:
            self.function_symbol(name)
            return True
        except KeyError:
            return False

    def call(self, expr: str, timeout=20):
        """Calls a function in the firmware, e.g. call('Bluetooth::queueCommand("U,Z")'). A Release ELF has no DWARF, so the
        function is looked up in the symbol table (nm) and called through a cast pointer; the return value as int (bool -> 0/1).
        It first halts at a safe point of the main loop (safe_bp), so that the call (and the gdb malloc for a string literal)
        never runs in the middle of an interrupted function. Returns int."""
        m = re.fullmatch(r"\s*([\w:]+)\s*\((.*)\)\s*", expr, re.S)
        if not m:
            raise ValueError(f"call: expected 'ns::func(args)', got {expr!r}")
        full = self.function_symbol(m.group(1))
        bp = self.c["safe_bp"]
        cmds = [f"hbreak '{self.function_symbol(bp)}'", "continue", "delete"] if self.has_function(bp) else []
        cmds.append(f"print (int)((int (*)())'{full}')({m.group(2)})")
        r = self._gdb(cmds, timeout)
        out = r.stdout + r.stderr
        errors = [l for l in out.splitlines() if re.search(r"No symbol|Cannot access|cannot be|^Error|error:|not defined|Too few|Invalid", l)
                  and "Python" not in l]
        val = re.search(r"^\$\d+ = (-?\d+)$", r.stdout, re.M)
        if r.returncode != 0 or errors or not val:
            raise RuntimeError(f"gdb call {expr} failed:\n" + out)
        return int(val.group(1))

    def function_address(self, name: str) -> int:
        full = self.function_symbol(name)
        nm = subprocess.run([NM, "-C", self.elf], capture_output=True, text=True).stdout
        for line in nm.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[2] == full:
                return int(parts[0], 16) & ~1
        raise KeyError(full)

    @contextlib.contextmanager
    def safe_halt(self, timeout_ms=3000):
        """Halts the core at a safe point of the main loop (HW breakpoint on safe_bp) for consistent writes to RAM
        (queues, state machine state); always resumes at the end."""
        addr = self.function_address(self.c["safe_bp"])
        self.tcl(f"capture {{bp 0x{addr:08x} 2 hw}}")
        try:
            self.tcl(f"capture {{wait_halt {timeout_ms}}}")
        finally:
            self.tcl(f"capture {{rbp 0x{addr:08x}}}")
        try:
            if not self.core_state()["halted"]:
                raise RuntimeError(f"{self.name}: safe point {self.c['safe_bp']} not reached")
            yield
        finally:
            self.tcl("capture {resume}")
            time.sleep(0.01)
            if self.core_state()["halted"]:
                self.resume()

    def inject_status(self, *lines: str):
        """Base: injects RN4870 status messages (text without '%') into Bluetooth::_statusQueue as if they came from the module.
        Slot size from nm -S (8 slots + head + tail)."""
        nm = subprocess.run([NM, "-C", "-S", self.elf], capture_output=True, text=True).stdout
        line = next(l for l in nm.splitlines() if l.endswith(" Bluetooth::_statusQueue"))
        addr, size = int(line.split()[0], 16), int(line.split()[1], 16)
        slot = (size - 8) // 8
        with self.safe_halt():
            head = int.from_bytes(self.read(addr + 8 * slot, 4), "little")
            for text in lines:
                data = text.encode()[: slot - 1] + b"\0"
                self.write(addr + head * slot, data)
                head = (head + 1) & 7
            self.write(addr + 8 * slot, head.to_bytes(4, "little"))

    def hold_ble_reset(self, on: bool):
        """RN4870 RST to zero (on=True, module held in reset) / back to one. A write to GPIO->BSRR while the core runs.
        Remote: PB8 (the firmware sets it only on (re)initialization of the module). Base: PA1; after a base reset its boot
        init() pulls it low itself and raises it after 100 ms, so holding it across a reset requires repeating the write (test_ble_down.py)."""
        if self.name == "remote":
            self.write32(REMOTE_GPIOB_BSRR, 1 << (REMOTE_BLE_RST_PIN + (16 if on else 0)))
        else:
            self.write32(BASE_GPIOA_BSRR, 1 << (BASE_BLE_RST_PIN + (16 if on else 0)))

    # -- remote: screens (struct fields need DWARF, the remote build is -g)
    def player_state(self) -> int:
        return self.u("GUI::playerScreen.state_", 1)

    def player_message(self) -> str:
        return self.cstr("GUI::playerScreen.message_")

    def current_screen(self) -> int:
        raw = self.read(self.sym("GUI::nav") + 4, 36)
        depth = int.from_bytes(raw[32:36], "little")
        return int.from_bytes(raw[4 * (depth - 1): 4 * depth], "little") if depth > 0 else 0

    def screen_name(self) -> str:
        cur = self.current_screen()
        for name in ("playerScreen", "mainScreen", "songListScreen", "pairingScreen", "splashScreen", "messageScreen", "settingsScreen"):
            try:
                if cur == self.sym(f"GUI::{name}"):
                    return name
            except KeyError:
                pass
        return hex(cur)

    def oled_sleeping(self) -> bool:
        return self.read_bool("Oled::sleeping")

    def type_code(self, code: str, enter=True):
        evs = []
        for ch in code:
            if ch.isdigit():
                evs.append(ch)
            else:
                evs += [MF] * (ord(ch) - ord("A") + 1)
        if enter:
            evs.append(ENTER)
        return self.key(*evs)

    def goto_player(self):
        """Remote: wake the OLED and get to the player screen (main menu, item 0)."""
        for _ in range(5):
            name = self.screen_name()
            if name == "playerScreen" and not self.oled_sleeping():
                return
            if self.oled_sleeping():
                self.key(BACK)
            elif name == "mainScreen":
                # item 0 = Prehravac; the selection may remain on "Nastaveni"
                sel = self.u("GUI::mainScreen.selectedIndex_")
                self.key(*([UP] * sel), ENTER)
            else:
                self.key(BACK)
            time.sleep(0.1)
        raise RuntimeError(f"could not reach the player screen (at {self.screen_name()})")

    def key(self, *events, timeout=20):
        """Remote: halt in the main loop, call GUI::keypress for each event, continue."""
        cmds = ["hbreak base::Keyboard::process", "continue", "delete", "call Oled::wakeupCallback()"]
        for e in events:
            n = DIGIT[e] if isinstance(e, str) else int(e)
            cmds.append(f"call GUI::keypress((ui::InputEvent){n})")
        r = self._gdb(cmds, timeout)
        out = r.stdout + r.stderr
        hit = re.search(r"Breakpoint 1, .*Keyboard::process", out) or re.search(r"Temporary breakpoint|Keyboard::process \(\)", out)
        errors = [l for l in out.splitlines() if re.search(r"No symbol|Cannot access|cannot be|^Error|error:", l) and "Python" not in l]
        if r.returncode != 0 or not hit or errors:
            raise RuntimeError("gdb key injection failed:\n" + out)
        return r.stdout

    def debug_log(self) -> list:
        """Base: content of the log in no-init RAM (Debug::_log = header {magic, head, tail, dropped, crc} + 1536 B ring) as
        lines from the oldest. head/tail are absolute positions (index = position % 1536)."""
        import struct
        size = 1536
        raw = self.read(self.sym("Debug::_log"), 20 + size)
        magic, head, tail, _dropped, _crc = struct.unpack("<5I", raw[:20])
        if magic != 0x4C4F4721 or (head - tail) & 0xFFFFFFFF > size:
            return []
        ring = raw[20:]
        data = bytes(ring[(tail + i) % size] for i in range((head - tail) & 0xFFFFFFFF))
        return [l.rstrip("\r") for l in data.decode(errors="replace").split("\n") if l.strip()]
