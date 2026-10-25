"""USB-MIDI on the PC side: `amidi` wrappers (ports/send/receive) and a timed reader of raw bytes from ALSA rawmidi.

Timing: `amidi` has no timestamps and `aseqdump` does not print them. `Reader` therefore reads /dev/snd/midiC<c>D<d> (rawmidi,
O_RDONLY) in a thread and adds time.monotonic_ns() to every return from read(). The USB-MIDI device is polled every 1 ms
and ALSA returns bytes in packets, so the resolution is ~1 ms (+ Python scheduler jitter, on the order of 100 us); that is
sufficient for the 30 ms threshold.
"""

import os
import re
import select
import subprocess
import threading
import time

BASE_NAME = "MIDIControl"
OTRAKI_NAME = "USB MIDI Interface"


def ports() -> dict:
    """{'base': 'hw:5,0,0', 'otraki': 'hw:4,0,0'} by name in `amidi -l`; a clear error when one is missing."""
    out = subprocess.run(["amidi", "-l"], capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines()[1:]:
        m = re.match(r"^\S+\s+(hw:\d+,\d+,\d+)\s+(.*?)\s*$", line)
        if not m:
            continue
        dev, name = m.groups()
        if OTRAKI_NAME in name:
            found.setdefault("otraki", dev)
        elif BASE_NAME in name:
            found.setdefault("base", dev)
    missing = [k for k in ("base", "otraki") if k not in found]
    if missing:
        raise RuntimeError(f"MIDI port not found: {missing} (looking for '{BASE_NAME}' / '{OTRAKI_NAME}'); amidi -l:\n{out}")
    return found


def send(port: str, hexstr: str):
    subprocess.run(["amidi", "-p", port, "-S", hexstr], check=True, timeout=5)


def receive(port: str, seconds: float) -> bytes:
    """Bytes received on the port within `seconds` (amidi -d -t)."""
    r = subprocess.run(["amidi", "-p", port, "-d", "-t", str(max(1, round(seconds)))], capture_output=True, text=True,
                       timeout=seconds + 10)
    return bytes.fromhex("".join(r.stdout.split()))


def rawpath(port: str) -> str:
    card, dev, _ = map(int, port[3:].split(","))
    return f"/dev/snd/midiC{card}D{dev}"


class Reader:
    """Thread reading raw bytes with timestamps: self.events = [(t_ns, bytes)] (one per read())."""

    def __init__(self, port: str):
        self.fd = os.open(rawpath(port), os.O_RDONLY | os.O_NONBLOCK)
        self.events = []
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self):
        while not self._stop.is_set():
            if select.select([self.fd], [], [], 0.05)[0]:
                try:
                    b = os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                if b:
                    self.events.append((time.monotonic_ns(), b))

    def close(self):
        self._stop.set()
        self._t.join(2)
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def data(self) -> bytes:
        return b"".join(b for _, b in list(self.events))

    def timed(self) -> list:
        """[(t_s, byte)] - all bytes of one read() have the same time."""
        return [(t / 1e9, x) for t, b in list(self.events) for x in b]


class Writer:
    """Direct write to rawmidi (without spawning a process, for precise timing)."""

    def __init__(self, port: str):
        self.fd = os.open(rawpath(port), os.O_WRONLY)

    def write(self, b: bytes):
        os.write(self.fd, b)

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def decode(data: bytes) -> list:
    """Bytes -> [messages] with running status expanded (realtime F8-FF as separate messages, SysEx as a whole)."""
    out, status, cur, sysex = [], 0, [], None
    need = {0x80: 2, 0x90: 2, 0xA0: 2, 0xB0: 2, 0xC0: 1, 0xD0: 1, 0xE0: 2}
    for b in data:
        if b >= 0xF8:
            out.append(bytes([b]))
        elif b == 0xF0:
            sysex = [b]
            status = 0
        elif b == 0xF7:
            if sysex is not None:
                out.append(bytes(sysex + [b]))
            sysex = None
        elif b >= 0x80:
            sysex = None
            status, cur = b, []
        elif sysex is not None:
            sysex.append(b)
        elif status:
            cur.append(b)
            if len(cur) == need[status & 0xF0]:
                out.append(bytes([status] + cur))
                cur = []
    return out
