# Kopie ../midicontroller-fw/tools/bletest/proto.py (repozitare jsou nezavisle); drzet v souladu s lib/protocol.
# DIAG_GET/DIAG_RESP + kodek zaznamu (protocol/diag.hpp) zrcadli lib/protocol; pri zmene upravit i bletest/proto.py.
"""midicontrol-protocol v2 codec (lib/protocol/PROTOCOL.md) for the BLE test harness."""

import struct
from dataclasses import dataclass
from enum import IntEnum

SOH, EOT, ESC = 0x01, 0x04, 0x1B
ESCAPES = {0x01: 0x21, 0x04: 0x22, 0x1B: 0x23, 0x24: 0x26, 0x25: 0x27}
UNESCAPES = {v: k for k, v in ESCAPES.items()}
SPECIAL = frozenset(ESCAPES)

ADDR_PC, ADDR_BASE, ADDR_REMOTE, ADDR_BCAST = 0x01, 0x02, 0x03, 0xFF
MAX_PAYLOAD = 128
RETRY_TIMEOUT = 0.5
MAX_RETRIES = 3


class MT(IntEnum):
    ACK = 0x01
    NAK = 0x02
    PLAY_SONG = 0x10
    STOP_SONG = 0x11
    RECORD_SONG = 0x12
    NOW_PLAYING_INFO = 0x13
    REPLAY_ANCHOR = 0x14
    GET_SONG_LIST = 0x20
    SONG_LIST_RESP = 0x21
    SIGNAL_PLAYING = 0x30
    SIGNAL_RECORDING = 0x31
    SIGNAL_STOPPED = 0x32
    SIGNAL_ANCHOR = 0x33
    SET_CURRENT = 0x40
    GET_STATUS = 0x50
    STATUS_RESP = 0x51
    KEEPALIVE = 0x60
    KEEPALIVE_ACK = 0x61
    FW_UPDATE_START = 0x70
    FW_UPDATE_DATA = 0x71
    FW_UPDATE_END = 0x72
    FW_UPDATE_STATUS = 0x73
    ENTER_BOOTLOADER = 0x74
    DIAG_GET = 0x80
    DIAG_RESP = 0x81


class NAK(IntEnum):
    UNKNOWN_MSG = 0x01
    INVALID_PAYLOAD = 0x02
    CRC_ERROR = 0x03
    BUSY = 0x04
    NOT_PLAYING = 0x05
    UNREACHABLE = 0x06
    NOT_FOUND = 0x07


# Diagnostic records (protocol/diag.hpp, PROTOCOL.md "Diagnostics"): 12 B little-endian
# seq u16, code u8, level u8, uptime_ms u32, arg u32; DIAG_RESP = [boot_id u16, next_cursor u16, n u8, n x record].
DIAG_RECORD_SIZE = 12
DIAG_MAX_RECORDS = 8
DIAG_CODES = {
    1: "BOOT", 2: "FAULT", 3: "FAULT_DETAIL", 4: "LOOP_STALL", 5: "IWDG_RESET", 6: "BLE_CONNECT",
    7: "BLE_DISCONNECT", 8: "BLE_SECURED", 9: "LINK_LOST", 10: "RETRIES_EXHAUSTED", 11: "CRC_ERRORS",
    12: "CHARGER_FAULT", 13: "LOW_BATTERY", 14: "OTA_RESULT", 15: "SHUTDOWN", 16: "RECORDS_LOST",
}
DIAG_LEVELS = {1: "ERROR", 2: "WARNING", 3: "INFO", 4: "DEBUG"}


def diag_encode_record(seq, code, level, uptime_ms, arg) -> bytes:
    return struct.pack("<HBBII", seq, code, level, uptime_ms, arg)


def diag_decode_record(raw: bytes):
    return struct.unpack("<HBBII", raw)


def diag_encode_get(cursor: int) -> bytes:
    return struct.pack("<H", cursor)


def diag_encode_resp(boot_id: int, next_cursor: int, records) -> bytes:
    assert len(records) <= DIAG_MAX_RECORDS
    return struct.pack("<HHB", boot_id, next_cursor, len(records)) + b"".join(diag_encode_record(*r) for r in records)


def diag_decode_resp(payload: bytes):
    boot_id, cursor, n = struct.unpack_from("<HHB", payload)
    if n > DIAG_MAX_RECORDS or len(payload) != 5 + n * DIAG_RECORD_SIZE:
        raise ValueError("bad DIAG_RESP payload length")
    recs = [diag_decode_record(payload[5 + i * DIAG_RECORD_SIZE:5 + (i + 1) * DIAG_RECORD_SIZE]) for i in range(n)]
    return boot_id, cursor, recs


@dataclass(frozen=True)
class Packet:
    src: int
    dst: int
    pid: int
    mtype: int
    payload: bytes = b""

    def __str__(self):
        try:
            name = MT(self.mtype).name
        except ValueError:
            name = f"0x{self.mtype:02X}"
        return f"{self.src}->{self.dst} #{self.pid} {name} {self.payload.hex(' ')}"


def crc8(data: bytes) -> int:
    """CRC-8 as all nodes implement it (lib/protocol/src/crc8.cpp table): poly 0x31, MSB first, init 0,
    no reflection; check value 0xA2. PROTOCOL.md calls it CRC-8/MAXIM, which would be the reflected form."""
    c = 0
    for x in data:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x31) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def escape(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b in ESCAPES:
            out += bytes([ESC, ESCAPES[b]])
        else:
            out.append(b)
    return bytes(out)


def raw_body(src, dst, pid, mtype, payload=b"", length=None) -> bytes:
    """Unescaped Length..Payload+CRC; `length` overrides the Length byte (for malformed-frame tests)."""
    body = bytes([4 + len(payload) if length is None else length, src, dst, pid & 0xFF, mtype]) + payload
    return body + bytes([crc8(body)])


def encode(src, dst, pid, mtype, payload=b"") -> bytes:
    return bytes([SOH]) + escape(raw_body(src, dst, pid, mtype, payload)) + bytes([EOT])


def encode_packet(p: Packet) -> bytes:
    return encode(p.src, p.dst, p.pid, p.mtype, p.payload)


class FrameParser:
    """Stream parser: frames SOH..EOT, everything outside frames goes to `.text` (RN4870 status, CMD> ...)."""

    MAX_BODY = 1 + 4 + MAX_PAYLOAD + 1

    def __init__(self):
        self.text = b""
        self.crc_errors = 0
        self.overflows = 0
        self.bad_frames = 0
        self._buf = None
        self._esc = False

    def feed(self, data: bytes) -> list:
        out = []
        for b in data:
            if b == SOH:
                self._buf, self._esc = bytearray(), False
                continue
            if self._buf is None:
                self.text += bytes([b])
                continue
            if b == EOT:
                pkt = self._finish(bytes(self._buf))
                self._buf = None
                if pkt:
                    out.append(pkt)
                continue
            if self._esc:
                self._esc = False
                if b not in UNESCAPES:
                    self.bad_frames += 1
                    self._buf = None
                    continue
                b = UNESCAPES[b]
            elif b == ESC:
                self._esc = True
                continue
            self._buf.append(b)
            if len(self._buf) > self.MAX_BODY:
                self.overflows += 1
                self._buf = None
        return out

    def _finish(self, body: bytes):
        if len(body) < 6 or body[0] != len(body) - 2:
            self.bad_frames += 1
            return None
        if crc8(body[:-1]) != body[-1]:
            self.crc_errors += 1
            return None
        return Packet(body[1], body[2], body[3], body[4], bytes(body[5:-1]))
