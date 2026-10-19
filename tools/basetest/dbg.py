"""ST-Link pomocnici pro base i remote: openocd (TCL, cteni bez zastaveni) + gdb (symboly, volani funkci).
Vzor: ../midicontroller-fw/tools/bletest/dbg.py. Jedna sonda = jeden openocd; dve sondy bezi soucasne."""

import atexit
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
                 gdb=3334, tcl=6668, telnet=6667, cwd=FW, gpio=0x48000000),
    "remote": dict(serial="37FF70064E59313840161343", cfg=os.path.join(REMOTE_FW, "openocd.cfg"),
                   elf=os.path.join(REMOTE_FW, "build", "midicontroller-fw.elf"), gdb=3333, tcl=6666, telnet=6665, cwd=REMOTE_FW, gpio=0x50000000),
}

# ui::InputEvent (remote)
UP, DOWN, MF, RIGHT, ENTER, BACK = 0, 1, 2, 3, 4, 5
DIGIT = {str(d): 6 + d for d in range(10)}

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

    def core_state(self) -> dict:
        dhcsr = int(self.tcl("read_memory 0xE000EDF0 32 1").split()[0], 0)
        return {"halted": bool(dhcsr & (1 << 17)), "sleeping": bool(dhcsr & (1 << 18)), "lockup": bool(dhcsr & (1 << 19)), "dhcsr": dhcsr}

    def resume(self):
        self.tcl("resume")

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
                q = f"'{name}'" if "(anonymous namespace)" in name and not name.startswith("'") else name
                self._sym[name] = int(re.search(r"0x[0-9a-f]+", self._static(f"&{q}")).group(0), 16)
        return self._sym[name]

    def u(self, name: str, size: int = 4) -> int:
        return int.from_bytes(self.read(self.sym(name), size), "little")

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
        """Base: obsah kruhoveho bufferu Debug::_logRing jako radky, od nejstarsiho (chrono podle _logHead)."""
        ring = self.read(self.sym("Debug::_logRing"), 512)
        head = self.u("Debug::_logHead")
        data = ring[head:] + ring[:head]
        # buffer jeste nepretekl: stary konec jsou nuly
        text = data.replace(b"\0", b"").decode(errors="replace")
        return [l for l in text.split("\n") if l]
