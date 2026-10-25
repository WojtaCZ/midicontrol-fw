"""PC side of the base USB-CDC link: pyserial + reader thread -> FrameParser -> packet queue.

The CDC carries only protocol frames; everything outside a frame ends up in `.stray` (must stay empty)."""

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
    """[(tty name, major:minor, bInterfaceNumber, interface string)] for ttyACM with VID:PID abcd:1234
    (no udev, sysfs only). The base has two CDCs: protocol (interface 00) and log ("MIDIControl log")."""
    found = []
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        itf = os.path.realpath(os.path.join(tty, "device"))
        d = itf
        while d.startswith("/sys/devices") and d != "/sys/devices":
            if _read(os.path.join(d, "idVendor")) == VID and _read(os.path.join(d, "idProduct")) == PID:
                found.append((os.path.basename(tty), _read(os.path.join(tty, "dev")), _read(os.path.join(itf, "bInterfaceNumber")),
                              _read(os.path.join(itf, "interface"))))
                break
            d = os.path.dirname(d)
    return found


PROTOCOL_ITF = "00"
LOG_ITF_NAME = "MIDIControl log"


def _node(name, majmin) -> str:
    """/dev/<name>; in a container without udev it creates it from the sysfs `dev` (sudo mknod)."""
    node = f"/dev/{name}"
    if majmin:
        major, minor = (int(x) for x in majmin.split(":"))
        if os.path.exists(node):
            st = os.stat(node)
            if (os.major(st.st_rdev), os.minor(st.st_rdev)) == (major, minor):
                return node
            subprocess.run(["sudo", "-n", "rm", "-f", node], check=True)
        subprocess.run(["sudo", "-n", "mknod", node, "c", str(major), str(minor)], check=True)
        subprocess.run(["sudo", "-n", "chmod", "666", node], check=True)
    return node


def find_port(interface: str = PROTOCOL_ITF, name: str = None) -> str:
    """Base CDC port: the protocol interface (bInterfaceNumber 00), or with `name` the interface matching the string
    (log CDC). Never returns an interface other than the requested one."""
    if name is None:
        by_id = sorted(glob.glob(f"/dev/serial/by-id/*ABCD_1234*-if{interface}"))
        if by_id:
            return by_id[0]
    for tty, majmin, ifnum, ifname in _sysfs_ports():
        if (name is not None and ifname == name) or (name is None and ifnum == interface):
            return _node(tty, majmin)
    what = f'interface "{name}"' if name else f"interface {interface}"
    raise RuntimeError(f"base (abcd:1234) {what} not found on USB")


class KeepaliveTracker:
    """Order of KEEPALIVEs sent to the base (the test's own and the background ones). The base answers in order and KEEPALIVE_ACK carries
    its own pid (it does not echo ours), so matching is FIFO: an ACK belongs to the oldest keepalive that has not expired yet.
    A background (bg) ACK is swallowed, a test one is passed on. Records older than `expiry` s are discarded."""

    def __init__(self, expiry=1.0):
        self.expiry = expiry
        self._q = []  # [(time, bg)]
        self._lock = threading.Lock()

    def sent(self, bg, now=None):
        with self._lock:
            self._q.append((time.monotonic() if now is None else now, bg))

    def ack(self, now=None) -> bool:
        """True = the ACK belongs to a background keepalive (swallow it)."""
        now = time.monotonic() if now is None else now
        with self._lock:
            self._q = [e for e in self._q if now - e[0] < self.expiry]
            if not self._q:
                return False
            return self._q.pop(0)[1]


class Link:
    def __init__(self, port=None, keepalive=False, keepalive_period=1.0, ser=None):
        self.port = port or (None if ser else find_port())
        self.ser = ser or serial.Serial(self.port, 115200, timeout=0.05, write_timeout=2)
        self.parser = proto.FrameParser()
        self._txparser = proto.FrameParser()  # observes our own writes for the keepalive bookkeeping
        self.stray = b""
        self.q = queue.Queue()
        self._backlog = []  # packets read from the queue that did not match the predicate
        self._pid = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self.paused = False  # True: do not send keepalive and do not answer the base's keepalive (the PC "goes silent")
        self.error = None  # exception from the thread; recv/send re-raise it
        self.keepalive_acks = 0
        self.keepalives_sent = 0
        self._ka_track = KeepaliveTracker()
        self.log = []  # (t, "tx"/"rx", Packet)
        self._reader = threading.Thread(target=self._guard, args=(self._read_loop,), daemon=True)
        self._reader.start()
        self._ka = None
        self._ka_period = keepalive_period
        if keepalive:
            self._ka = threading.Thread(target=self._guard, args=(self._ka_loop, keepalive_period), daemon=True)
            self._ka.start()

    def _guard(self, fn, *args):
        try:
            fn(*args)
        except BaseException as e:  # noqa: BLE001 - record it and surface it to the test
            self.error = e

    def _check(self):
        if self.error is not None:
            raise RuntimeError(f"usblink thread failed: {self.error!r}") from self.error

    def pause_keepalive(self, paused=True):
        self.paused = paused

    # -- threads
    def _read_loop(self):
        while not self._stop.is_set():
            try:
                data = self.ser.read(256)
            except (serial.SerialException, OSError):
                if self._stop.is_set():
                    return
                raise
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
                    if not self.paused:
                        self.send_raw(proto.encode(proto.ADDR_PC, p.src, p.pid, proto.MT.KEEPALIVE_ACK))
                    # ...and also deliver it to the test (recv with pred)
                elif p.mtype == proto.MT.KEEPALIVE_ACK and self._ka_track.ack():
                    self.keepalive_acks += 1
                    continue
                self.q.put(p)

    def _ka_loop(self, period):
        while not self._stop.wait(period):
            if self.paused:
                continue
            self.keepalives_sent += 1
            self._send(proto.Packet(proto.ADDR_PC, proto.ADDR_BASE, 0, proto.MT.KEEPALIVE), bg=True)

    # -- API
    def next_pid(self) -> int:
        with self._lock:
            self._pid = (self._pid + 1) & 0xFF
            return self._pid

    def send_raw(self, b: bytes, chunk=None):
        self._check()
        with self._lock:
            self._observe(b)
            if chunk:
                for i in range(0, len(b), chunk):
                    self.ser.write(b[i : i + chunk])
                    self.ser.flush()
            else:
                self.ser.write(b)
                self.ser.flush()

    def _observe(self, b):
        """Remembers keepalives for the base in our own writes (raw ones too), so that the ACK matching works out."""
        for p in self._txparser.feed(b):
            if p.dst == proto.ADDR_BASE and p.mtype == proto.MT.KEEPALIVE:
                self._ka_track.sent(False)

    def _send(self, pkt, bg=False):
        pid = self.next_pid()
        self.log.append((time.monotonic(), "tx", pkt))
        frame = proto.encode(pkt.src, pkt.dst, pid, pkt.mtype, pkt.payload)
        self._check()
        with self._lock:
            if bg:
                self._ka_track.sent(True)
                self._txparser.feed(frame)  # do not count it a second time
            else:
                self._observe(frame)
            self.ser.write(frame)
            self.ser.flush()
        return pid

    def send(self, pkt: proto.Packet) -> int:
        """Sends a packet; the pid is filled in from a single counter. Returns the pid used."""
        return self._send(pkt)

    def send_msg(self, mtype, payload=b"", dst=proto.ADDR_BASE) -> int:
        return self.send(proto.Packet(proto.ADDR_PC, dst, 0, mtype, payload))

    def _take(self, pred):
        for i, p in enumerate(self._backlog):
            if pred is None or pred(p):
                return self._backlog.pop(i)
        return None

    def recv(self, timeout: float, pred=None):
        """First packet that satisfies pred (None = any). The others remain for the next recv()."""
        end = time.monotonic() + timeout
        while True:
            self._check()
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
        """Discards everything that has arrived; returns it."""
        out = list(self._backlog)
        self._backlog.clear()
        while True:
            try:
                out.append(self.q.get_nowait())
            except queue.Empty:
                return out

    def expect_silence(self, seconds: float) -> list:
        """Waits `seconds`; returns everything that arrived during that time (and before) - an empty list = silence."""
        time.sleep(seconds)
        return self.drain()

    def detach(self):
        """Stops the threads and closes the port (before a base reset, when the CDC disappears). Then attach()."""
        self._stop.set()
        self._reader.join(2)
        if self._ka:
            self._ka.join(2)
        try:
            self.ser.close()
        except Exception:
            pass

    def attach(self, timeout=15.0, settle=2.0):
        """After detach(): waits for the CDC to be enumerated again, opens it and restarts the threads (keepalive, if enabled).
        `settle`: first wait for the old CDC to disappear (reset -> bootloader -> application), otherwise the old
        /dev/ttyACMn is opened and the read then ends with SerialException."""
        time.sleep(settle)
        end = time.monotonic() + timeout
        last = None
        while time.monotonic() < end:
            try:
                self.port = find_port()
                self.ser = serial.Serial(self.port, 115200, timeout=0.05, write_timeout=2)
                break
            except Exception as e:  # noqa: BLE001 - the port is not there yet
                last = e
                time.sleep(0.3)
        else:
            raise RuntimeError(f"CDC did not come back within {timeout} s: {last!r}")
        self.parser = proto.FrameParser()
        self._txparser = proto.FrameParser()
        self._ka_track = KeepaliveTracker()
        self._stop.clear()
        self._reader = threading.Thread(target=self._guard, args=(self._read_loop,), daemon=True)
        self._reader.start()
        if self._ka:
            self._ka = threading.Thread(target=self._guard, args=(self._ka_loop, self._ka_period), daemon=True)
            self._ka.start()

    def close(self):
        self._stop.set()
        self._reader.join(1)
        try:
            self.ser.close()
        except Exception:
            pass
