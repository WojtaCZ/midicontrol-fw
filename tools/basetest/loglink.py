"""PC strana log CDC base ("MIDIControl log"): textove radky `[%6lu.%03lu] L tag: text\\r\\n`.

Ctecí vlakno sklada prijata data do celych radku (`.history`); `lines`/`wait_for` ctou od kurzoru dal.
Odpovedi na prikazy (`level d`, `dump`, `help`) jsou radky bez log prefixu."""

import re
import threading
import time

import serial

import usblink

LINE_RE = re.compile(r"^\[ *\d+\.\d{3}\] [EWID] [a-z]{1,6}: .{0,120}~?$")
MARKER_RE = re.compile(r"^(--- .* ---|\[dropped \d+\])$")


def is_log_line(line: str) -> bool:
    return bool(LINE_RE.match(line))


def is_marker(line: str) -> bool:
    return bool(MARKER_RE.match(line))


def find_log_port() -> str:
    return usblink.find_port(name=usblink.LOG_ITF_NAME)


class LogLink:
    def __init__(self, port=None, ser=None):
        self.port = port or (None if ser else find_log_port())
        self.ser = ser or serial.Serial(self.port, 115200, timeout=0.05, write_timeout=2)
        self.history = []  # [(cas prijmu, radek)] vsechny cele radky
        self.raw = b""
        self._buf = b""
        self._pos = 0  # kurzor pro lines()/wait_for()
        self._cv = threading.Condition()
        self._stop = threading.Event()
        self.error = None
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        try:
            while not self._stop.is_set():
                try:
                    data = self.ser.read(512)
                except (serial.SerialException, OSError):
                    if self._stop.is_set():
                        return
                    raise
                if not data:
                    continue
                with self._cv:
                    self.raw += data
                    self._buf += data
                    while b"\n" in self._buf:
                        line, self._buf = self._buf.split(b"\n", 1)
                        self.history.append((time.monotonic(), line.rstrip(b"\r").decode(errors="replace")))
                    self._cv.notify_all()
        except BaseException as e:  # noqa: BLE001 - ukazat testu
            self.error = e

    def _check(self):
        if self.error is not None:
            raise RuntimeError(f"loglink thread failed: {self.error!r}") from self.error

    def _next(self, deadline):
        """Dalsi radek od kurzoru, nebo None po deadline."""
        with self._cv:
            while self._pos >= len(self.history):
                self._check()
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self._cv.wait(min(left, 0.1))
            line = self.history[self._pos][1]
            self._pos += 1
            return line

    def skip(self):
        """Kurzor na konec: dalsi lines()/wait_for() vidi jen nove radky."""
        with self._cv:
            self._pos = len(self.history)

    def lines(self, timeout: float, pred=None) -> list:
        """Cele radky prijate behem `timeout` (od kurzoru). S `pred` skonci drive na prvnim radku, kde pred(radek)."""
        end = time.monotonic() + timeout
        out = []
        while True:
            line = self._next(end)
            if line is None:
                return out
            out.append(line)
            if pred is not None and pred(line):
                return out

    def wait_for(self, regex: str, timeout: float) -> re.Match:
        """Prvni radek od kurzoru, ktery odpovida `regex` (re.search); jinak AssertionError."""
        rx = re.compile(regex)
        end = time.monotonic() + timeout
        seen = []
        while True:
            line = self._next(end)
            if line is None:
                tail = "\n".join(seen[-15:])
                raise AssertionError(f"log: {regex!r} not seen within {timeout} s; last lines:\n{tail}")
            seen.append(line)
            m = rx.search(line)
            if m:
                return m

    def command(self, text: str, timeout: float = 1.5, quiet: float = 0.3) -> list:
        """Posle prikaz + CRLF; vraci radky odpovedi (radky bez log prefixu a mimo znacky). Log radky prijate mezitim
        zustavaji v historii (kurzor se jimi neposouva)."""
        with self._cv:
            start = len(self.history)
        self.ser.write(text.encode() + b"\r\n")
        self.ser.flush()
        end = time.monotonic() + timeout
        replies = []
        last = None
        while time.monotonic() < end:
            self._check()
            with self._cv:
                new = [l for _, l in self.history[start:]]
            replies = [l for l in new if not is_log_line(l) and not is_marker(l)]
            if replies:
                if last is None or len(replies) != last[0]:
                    last = (len(replies), time.monotonic())
                elif time.monotonic() - last[1] > quiet:
                    break
            time.sleep(0.05)
        return replies

    def close(self):
        self._stop.set()
        self._reader.join(1)
        try:
            self.ser.close()
        except Exception:
            pass
