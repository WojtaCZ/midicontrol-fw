"""PC strana USB-CDC linku base: pyserial + ctecí vlakno -> FrameParser -> fronta paketu.

CDC nese jen protokolove ramce; vse mimo ramec konci v `.stray` (musi zustat prazdne)."""

import glob
import os
import queue
import subprocess
import threading
import time

import serial

import proto

VID, PID = "abcd", "1234"


def _read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def _sysfs_ports():
    """[(tty jmeno, major:minor)] pro ttyACM s VID:PID abcd:1234 (bez udev, jen sysfs)."""
    found = []
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        d = os.path.realpath(os.path.join(tty, "device"))
        while d.startswith("/sys/devices") and d != "/sys/devices":
            if _read(os.path.join(d, "idVendor")) == VID and _read(os.path.join(d, "idProduct")) == PID:
                found.append((os.path.basename(tty), _read(os.path.join(tty, "dev"))))
                break
            d = os.path.dirname(d)
    return found


def find_port() -> str:
    by_id = sorted(glob.glob("/dev/serial/by-id/*ABCD_1234*"))
    if by_id:
        return by_id[0]
    for name, majmin in _sysfs_ports():
        node = f"/dev/{name}"
        if not os.path.exists(node) and majmin:
            major, minor = majmin.split(":")
            subprocess.run(["sudo", "-n", "mknod", node, "c", major, minor], check=True)
            subprocess.run(["sudo", "-n", "chmod", "666", node], check=True)
        return node
    raise RuntimeError("base (abcd:1234) not found on USB")


class Link:
    def __init__(self, port=None, keepalive=False, keepalive_period=1.0):
        self.port = port or find_port()
        self.ser = serial.Serial(self.port, 115200, timeout=0.05, write_timeout=2)
        self.parser = proto.FrameParser()
        self.stray = b""
        self.q = queue.Queue()
        self._backlog = []  # pakety prectene z fronty, ktere nesedely na predikat
        self._pid = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self.keepalive_acks = 0
        self.keepalives_sent = 0
        self._ka_pending = 0
        self.log = []  # (t, "tx"/"rx", Packet)
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._ka = None
        if keepalive:
            self._ka = threading.Thread(target=self._ka_loop, args=(keepalive_period,), daemon=True)
            self._ka.start()

    # -- vlakna
    def _read_loop(self):
        while not self._stop.is_set():
            try:
                data = self.ser.read(256)
            except (serial.SerialException, OSError):
                return
            if not data:
                continue
            before = len(self.parser.text)
            pkts = self.parser.feed(data)
            if len(self.parser.text) > before:
                self.stray += self.parser.text[before:]
                self.parser.text = b""
            for p in pkts:
                self.log.append((time.monotonic(), "rx", p))
                if p.src == proto.ADDR_BASE and p.mtype == proto.MT.KEEPALIVE:
                    self.send_raw(proto.encode(proto.ADDR_PC, p.src, p.pid, proto.MT.KEEPALIVE_ACK))
                    continue
                if p.mtype == proto.MT.KEEPALIVE_ACK and self._ka_pending > 0:
                    self._ka_pending -= 1  # odpoved na nas pozadovy keepalive
                    self.keepalive_acks += 1
                    continue
                self.q.put(p)

    def _ka_loop(self, period):
        while not self._stop.wait(period):
            try:
                self._ka_pending += 1
                self.keepalives_sent += 1
                self.send(proto.Packet(proto.ADDR_PC, proto.ADDR_BASE, 0, proto.MT.KEEPALIVE))
            except Exception:
                return

    # -- API
    def next_pid(self) -> int:
        with self._lock:
            self._pid = (self._pid + 1) & 0xFF
            return self._pid

    def send_raw(self, b: bytes, chunk=None):
        with self._lock:
            if chunk:
                for i in range(0, len(b), chunk):
                    self.ser.write(b[i : i + chunk])
                    self.ser.flush()
            else:
                self.ser.write(b)
                self.ser.flush()

    def send(self, pkt: proto.Packet) -> int:
        """Odesle paket; pid se vyplni z jednoho citace. Vraci pouzity pid."""
        pid = self.next_pid()
        self.log.append((time.monotonic(), "tx", pkt))
        self.send_raw(proto.encode(pkt.src, pkt.dst, pid, pkt.mtype, pkt.payload))
        return pid

    def send_msg(self, mtype, payload=b"", dst=proto.ADDR_BASE) -> int:
        return self.send(proto.Packet(proto.ADDR_PC, dst, 0, mtype, payload))

    def _take(self, pred):
        for i, p in enumerate(self._backlog):
            if pred is None or pred(p):
                return self._backlog.pop(i)
        return None

    def recv(self, timeout: float, pred=None):
        """Prvni paket, ktery splnuje pred (None = jakykoli). Ostatni zustavaji pro dalsi recv()."""
        end = time.monotonic() + timeout
        while True:
            p = self._take(pred)
            if p:
                return p
            left = end - time.monotonic()
            if left <= 0:
                return None
            try:
                self._backlog.append(self.q.get(timeout=left))
            except queue.Empty:
                return None

    def drain(self) -> list:
        """Zahodi vse, co dorazilo; vraci to."""
        out = list(self._backlog)
        self._backlog.clear()
        while True:
            try:
                out.append(self.q.get_nowait())
            except queue.Empty:
                return out

    def expect_silence(self, seconds: float) -> list:
        """Ceka `seconds`; vraci vse, co za tu dobu (a driv) dorazilo - prazdny seznam = ticho."""
        time.sleep(seconds)
        return self.drain()

    def close(self):
        self._stop.set()
        self._reader.join(1)
        try:
            self.ser.close()
        except Exception:
            pass
