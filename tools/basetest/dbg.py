"""ST-Link pomocnici pro base i remote: openocd (TCL, cteni bez zastaveni) + gdb (symboly, volani funkci).
Vzor: ../midicontroller-fw/tools/bletest/dbg.py. Jedna sonda = jeden openocd; dve sondy bezi soucasne."""

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
                   # Release ELF remote nema DWARF; BASETEST_REMOTE_ELF = -g kopie se stejnym kodem (README)
                   elf=os.environ.get("BASETEST_REMOTE_ELF", os.path.join(REMOTE_FW, "build", "midicontroller-fw.elf")), gdb=3333, tcl=6666, telnet=6665, cwd=REMOTE_FW, gpio=0x50000000,
                   safe_bp="base::Keyboard::process"),
}

# fault::Record (base, inc/fault.hpp): 8 B hlavicka + 18 x u32 + rev[8] + crc
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
# Remote RN4870 RST = PB8 (aktivni v nule); GPIOB BSRR na G071
REMOTE_GPIOB_BSRR = 0x50000400 + 0x18
REMOTE_BLE_RST_PIN = 8

def probe_holder(name: str):
    """Cizi proces (openocd jine session), ktery drzi sondu `name`; None = volna nebo nase."""
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


def stop_all():
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
        for _ in range(300):  # prvni pripojeni k sonde muze trvat i 10 s
            if _servers[self.name].poll() is not None:
                if _retry:  # obcas probe po predchozim pripojeni jeste neni pripravena
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
        p = _servers.pop(self.name, None)
        if p and p.poll() is None:
            p.terminate()
            p.wait(5)

    def tcl(self, cmd: str) -> str:
        self.start()
        with socket.create_connection(("127.0.0.1", self.c["tcl"]), 5) as s:
            s.sendall(cmd.encode() + b"\x1a")
            buf = b""
            while not buf.endswith(b"\x1a"):
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
        return buf[:-1].decode(errors="replace")

    def read(self, addr: int, n: int) -> bytes:
        """Cteni pameti bez zastaveni jadra (AHB-AP)."""
        out = self.tcl(f"read_memory 0x{addr:08x} 8 {n}")
        return bytes(int(x, 0) for x in out.split())

    def write(self, addr: int, data: bytes):
        """Zapis do pameti bez zastaveni jadra (AHB-AP), napr. pro volatile priznak."""
        self.tcl(f"write_memory 0x{addr:08x} 8 {{{' '.join(f'0x{b:02x}' for b in data)}}}")

    def core_state(self) -> dict:
        dhcsr = int(self.tcl("read_memory 0xE000EDF0 32 1").split()[0], 0)
        return {"halted": bool(dhcsr & (1 << 17)), "sleeping": bool(dhcsr & (1 << 18)), "lockup": bool(dhcsr & (1 << 19)), "dhcsr": dhcsr}

    def resume(self):
        # samotne "resume" openocd ignoruje, kdyz o zastaveni nevi (napr. novy openocd) -> nejdriv halt
        self.tcl("capture {halt}; capture {resume}")

    def pc_if_halted(self):
        return self.tcl("reg pc") if self.core_state()["halted"] else None

    # -- symboly (jen z ELF)
    def _static(self, expr: str) -> str:
        out = subprocess.run([GDB, "-q", "-nx", "-batch", "-ex", f"print {expr}", self.elf], capture_output=True, text=True).stdout
        m = re.search(r"^\$1 = (.*)$", out, re.M)
        if not m:
            raise KeyError(f"{expr}: {out}")
        return m.group(1)

    def sym(self, name: str) -> int:
        """Adresa symbolu, napr. 'Debug::_logRing'. Nejdriv tabulka symbolu (nm -C, funguje i bez DWARF),
        jinak gdb (pole struktur, napr. 'GUI::playerScreen.state_', chce -g)."""
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
        """Staticke promenne, ktere LTO prejmenuje na <mangled>.lto_priv.N (nm -C je pak nedemangluje, zalezi na
        rozdeleni LTO partition - zmena jinde ve firmware ji muze vyvolat). Hleda se demanglovany zaklad jmena."""
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
        return int.from_bytes(self.read(self.sym(name), size), "little")

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
        """Stav pinu z IDR (port 'A'..): G431 GPIO na 0x48000000, G071 na 0x50000000, krok 0x400."""
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
        # nikdy nenechat jadro zastavene: resume pred detach
        args += ["-ex", "monitor resume", "-ex", "detach"]
        try:
            r = subprocess.run(args + [self.elf], capture_output=True, text=True, timeout=timeout)
        finally:
            if self.core_state()["halted"]:
                self.resume()
        return r

    def reset(self):
        """Reset jadra pres sondu (reset run); jadro se nikdy nenecha zastavene."""
        self.tcl("reset run")
        time.sleep(0.2)
        if self.core_state()["halted"]:
            self.resume()

    def release_debug(self):
        """Odpojit ladeni tak, aby firmware videlo DHCSR.C_DEBUGEN = 0 (fault -> zaznam + NVIC_SystemReset misto bkpt).
        C_DEBUGEN nuluje jen POR nebo zapis DHCSR; ST-Link (hla) ho pri pripojeni nastavi a systemovy reset ho necha.
        Proto: DHCSR = DBGKEY bez C_DEBUGEN (0xA05F0000, jadro bezi dal), hned potom ukoncit openocd (dalsi tcl() ho
        spusti znovu a C_DEBUGEN opet nastavi). Viz README "Fault tests"."""
        self.tcl("write_memory 0xE000EDF0 32 {0xA05F0000}")
        self.stop()

    def addr2line(self, addr: int) -> str:
        """Jmeno funkce pro adresu (arm-none-eabi-addr2line -f -C); Release ELF bez DWARF -> z tabulky symbolu."""
        r = subprocess.run([NM.replace("nm", "addr2line"), "-f", "-C", "-e", self.elf, f"0x{addr:08x}"], capture_output=True, text=True)
        return r.stdout.splitlines()[0] if r.stdout else "??"

    def fault_record(self):
        """Base: fault::_record v .noinit jako dict, nebo None (spatna magic/CRC). Rozlozeni = fault::Record (fault.hpp)."""
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
        """Plny demanglovany nazev funkce z tabulky symbolu, napr. 'Bluetooth::queueCommand(char const*)'."""
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
        """Zavola funkci ve firmware, napr. call('Bluetooth::queueCommand("U,Z")'). Release ELF nema DWARF, takze se
        funkce hleda v tabulce symbolu (nm) a vola pres pretypovany ukazatel; navratova hodnota jako int (bool -> 0/1).
        Nejdriv zastavi v bezpecnem miste hlavni smycky (safe_bp), aby volani (a gdb malloc pro retezcovy literal)
        nikdy neprobehlo uprostred prerusene funkce. Vraci int."""
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
        """Zastavi jadro v bezpecnem miste hlavni smycky (HW breakpoint na safe_bp) pro konzistentni zapisy do RAM
        (fronty, stav automatu); na konci vzdy pokracuje."""
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
        """Base: vlozi stavove zpravy RN4870 (text bez '%') do Bluetooth::_statusQueue, jako by prisly z modulu.
        Velikost slotu z nm -S (8 slotu + head + tail)."""
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
        """Remote: RN4870 RST (PB8) do nuly (on=True, modul v resetu) / zpet do jednicky. Zapis do GPIOB->BSRR
        za behu jadra; firmware PB8 jinak nastavuje jen pri (re)inicializaci modulu."""
        assert self.name == "remote"
        bit = REMOTE_BLE_RST_PIN + (16 if on else 0)
        self.write32(REMOTE_GPIOB_BSRR, 1 << bit)

    # -- remote: obrazovky (pole struktur potrebuji DWARF, remote build je -g)
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
        """Remote: probudit OLED a dostat se na obrazovku prehravace (hlavni menu, polozka 0)."""
        for _ in range(5):
            name = self.screen_name()
            if name == "playerScreen" and not self.oled_sleeping():
                return
            if self.oled_sleeping():
                self.key(BACK)
            elif name == "mainScreen":
                # polozka 0 = Prehravac; vyber muze zustat na "Nastaveni"
                sel = self.u("GUI::mainScreen.selectedIndex_")
                self.key(*([UP] * sel), ENTER)
            else:
                self.key(BACK)
            time.sleep(0.1)
        raise RuntimeError(f"could not reach the player screen (at {self.screen_name()})")

    def key(self, *events, timeout=20):
        """Remote: zastavit v hlavni smycce, volat GUI::keypress pro kazdou udalost, pokracovat."""
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
        """Base: obsah logu v no-init RAM (Debug::_log = hlavicka {magic, head, tail, dropped, crc} + 1536 B ring) jako
        radky od nejstarsiho. head/tail jsou absolutni pozice (index = pozice % 1536)."""
        import struct
        size = 1536
        raw = self.read(self.sym("Debug::_log"), 20 + size)
        magic, head, tail, _dropped, _crc = struct.unpack("<5I", raw[:20])
        if magic != 0x4C4F4721 or (head - tail) & 0xFFFFFFFF > size:
            return []
        ring = raw[20:]
        data = bytes(ring[(tail + i) % size] for i in range((head - tail) & 0xFFFFFFFF))
        return [l.rstrip("\r") for l in data.decode(errors="replace").split("\n") if l.strip()]
