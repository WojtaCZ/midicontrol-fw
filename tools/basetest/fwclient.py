"""FW_UPDATE_* client of the harness (like ../midicontrolpy-sw/src/firmware_update.cpp, but with exact control over what and
when is sent): stopping after N bytes, altered images. Transport = anything with send(Packet)/recv(timeout, pred)/drain(),
i.e. usblink.Link (base application, remote via the base) or a Link on the base bootloader port (abcd:1235)."""

import glob
import os
import struct
import sys
import time
import zlib

import serial

import proto
import usblink
from proto import MT, Packet

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "lib", "bootloader", "tools"))
import fw_trailer  # noqa: E402

CHUNK = 120  # same as the PC (multiple of 8 because of the base)
ST_READY, ST_IN_PROGRESS, ST_CRC_OK, ST_CRC_FAIL, ST_WRITE_ERROR, ST_BAD_SIZE = range(6)
BOOTLOADER_PID = "1235"


def start_payload(target: int, size: int, crc: int) -> bytes:
    return bytes([target]) + struct.pack("<II", size, crc)


def data_payload(offset: int, chunk: bytes) -> bytes:
    return struct.pack("<I", offset) + chunk


def offset_at(size: int, pct: int) -> int:
    """Number of bytes sent at `pct` %: rounded down to whole chunks, 100 % = everything."""
    if pct >= 100:
        return size
    return (size * pct // 100) // CHUNK * CHUNK


def make_image(update: bytes, *, board=None, version=None, corrupt_trailer=False, corrupt_body=False, truncate=None) -> bytes:
    """From a valid update file: a different board or version (re-signed), a corrupted trailer, a corrupted body (original
    trailer), truncated by `truncate` bytes."""
    size, _crc, ver, b, _fmt, _r, _tcrc, _magic = struct.unpack("<IIIBBHII", update[-fw_trailer.TRAILER_SIZE:])
    body = bytearray(update[:size])
    out = bytes(body) + fw_trailer.make_trailer(bytes(body), board if board is not None else b, version if version is not None else ver)
    if corrupt_body:
        body[len(body) // 2] ^= 0xFF
        out = bytes(body) + update[size:]
    if corrupt_trailer:
        t = bytearray(out[-fw_trailer.TRAILER_SIZE:])
        t[8] ^= 0x01  # version: trailerCrc then does not match
        out = out[:-fw_trailer.TRAILER_SIZE] + bytes(t)
    if truncate:
        out = out[:-truncate]
    return out


def bootloader_port(timeout=10.0) -> str:
    """Protocol port of the base bootloader (abcd:1235, interface 00); waits until it can be opened (the /dev node in the container
    appears only ~0.2 s after sysfs)."""
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
            d = os.path.realpath(os.path.join(tty, "device"))
            itf = d
            while d.startswith("/sys/devices") and d != "/sys/devices":
                if usblink._read(os.path.join(d, "idVendor")) == usblink.VID and usblink._read(os.path.join(d, "idProduct")) == BOOTLOADER_PID:
                    node = f"/dev/{os.path.basename(tty)}"
                    try:
                        serial.Serial(node, 115200, timeout=0.05).close()
                        if usblink._read(os.path.join(itf, "bInterfaceNumber")) == "00":
                            return node
                    except Exception as e:  # noqa: BLE001 - the node is not ready yet
                        last = e
                    break
                d = os.path.dirname(d)
        time.sleep(0.05)
    raise RuntimeError(f"bootloader port (abcd:1235) not openable within {timeout} s: {last!r}")


class Client:
    def __init__(self, link, target: int, retries: int = 3, src: int = proto.ADDR_PC):
        self.link = link
        self.target = target
        self.retries = retries
        self.src = src

    def _reliable(self, mtype, payload=b"", timeout=2.0, status_wait=0.5):
        """(acked, status|None): ACK/NAK with the pid in payload[0]; STATUS, if it arrives within `status_wait` s after the ACK (DATA gets
        STATUS only on error, which comes right after the NAK/ACK)."""
        for _ in range(self.retries + 1):
            pid = self.link.send(Packet(self.src, self.target, 0, mtype, payload))
            reply = self.link.recv(timeout, lambda p: p.src == self.target and p.mtype in (MT.ACK, MT.NAK) and p.payload[:1] == bytes([pid]))
            if reply is None:
                continue
            st = self.link.recv(status_wait if reply.mtype == MT.ACK else 0.2, lambda p: p.src == self.target and p.mtype == MT.FW_UPDATE_STATUS)
            status = st.payload[0] if st is not None and st.payload else None
            return reply.mtype == MT.ACK, status
        return False, None

    def start(self, size: int, crc: int, timeout=15.0, target_byte=None):
        return self._reliable(MT.FW_UPDATE_START, start_payload(self.target if target_byte is None else target_byte, size, crc), timeout)

    def data(self, off: int, chunk: bytes, timeout=2.0):
        return self._reliable(MT.FW_UPDATE_DATA, data_payload(off, chunk), timeout, status_wait=0.01)

    def end(self, timeout=10.0):
        return self._reliable(MT.FW_UPDATE_END, b"", timeout)

    def send_image(self, img: bytes, stop_after=None, on_chunk=None, target_byte=None) -> str:
        """"ok" | "stopped" (after `stop_after` bytes, END is not sent) | error text."""
        self.link.drain()
        acked, st = self.start(len(img), zlib.crc32(img), target_byte=target_byte)
        if not acked:
            return f"START NAK status {st}"
        if st is not None and st != ST_READY:
            return f"START status 0x{st:02x}"
        for off in range(0, len(img), CHUNK):
            if stop_after is not None and off >= stop_after:
                return "stopped"
            if on_chunk:
                on_chunk(off, len(img))
            acked, st = self.data(off, img[off:off + CHUNK])
            if not acked:
                return f"DATA NAK at {off} status {st}"
            if st is not None and st not in (ST_READY, ST_IN_PROGRESS):
                return f"DATA status 0x{st:02x} at {off}"
        if stop_after is not None:
            return "stopped"
        acked, st = self.end()
        if not acked:
            return f"END NAK status {st}"
        if st != ST_CRC_OK:
            return f"END status 0x{st:02x}" if st is not None else "END no status"
        return "ok"
