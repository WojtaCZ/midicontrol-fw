"""Output to the church display: NOW_PLAYING_INFO over USB-CDC -> a 9-byte frame on the display UART (1200 Bd, D1 = MCU -> display)."""

import time

import pytest

import la
import proto
from proto import MT

ADDR_BASE = proto.ADDR_BASE

SAMPLERATE = 100_000
BAUD = 1200
BLANK = 0xE0
RED, YELLOW, GREEN, BLUE = 0x20, 0x21, 0x22, 0x23
BLANK_FRAME = bytes([0xB0] + [BLANK] * 8)


def expected_frame(song_h, song_l, verse, letter, led, prev=BLANK_FRAME) -> bytes:
    """Expected display frame from a NOW_PLAYING_INFO payload; `prev` = the frame before the message (for "unchanged" fields).

    Layout (src/display.cpp, currentState; each byte 0xE0 = blanked, digits 0x00-0x09):
      [0] 0xB0 frame marker
      [1] verse - units         [2] verse - tens
      [3] song - units          [4] song - tens
      [5] song - hundreds       [6] song - thousands
      [7] letter: 0x0A-0x0D = A-D
      [8] LED: 0x20 red, 0x21 yellow, 0x22 green, 0x23 blue
    Payload rules (PROTOCOL.md): nibble > 9 = blank digit; 0xFF = unchanged (song only when both song_h and song_l
    are 0xFF); 0xFE in song_h or verse = blank the field.
    """
    f = bytearray(prev)

    def digit(n):
        return n if n <= 9 else BLANK

    if song_h == 0xFE:
        f[3:7] = bytes([BLANK] * 4)
    elif not (song_h == 0xFF and song_l == 0xFF):
        f[6], f[5], f[4], f[3] = digit(song_h >> 4), digit(song_h & 15), digit(song_l >> 4), digit(song_l & 15)
    if verse == 0xFE:
        f[1] = f[2] = BLANK
    elif verse != 0xFF:
        f[2], f[1] = digit(verse >> 4), digit(verse & 15)
    if letter != 0xFF:
        f[7] = letter if 0x0A <= letter <= 0x0D else BLANK
    if led != 0xFF:
        f[8] = {1: RED, 2: GREEN, 3: BLUE, 4: YELLOW}.get(led, BLANK)
    return bytes(f)


def split_frames(data):
    """[(time, byte)] -> [(time_of_first_byte, bytes)]. A frame starts with the marker 0xB0 (no other frame byte ever has it);
    splitting by a time gap does not work, because on a change during transmission the base sends the next frame right after the previous one."""
    frames = []
    for t, b in data:
        if b == 0xB0:
            frames.append((t, bytearray()))
        if frames:  # bytes before the first 0xB0 are a truncated start of the capture (the capture may start in the middle of a frame)
            frames[-1][1].append(b)
    return [(t, bytes(b)) for t, b in frames]


@pytest.fixture
def quiet(link):
    link.pause_keepalive(True)
    link.drain()
    try:
        yield link
    finally:
        link.pause_keepalive(False)


@pytest.fixture
def display_on(base, quiet):
    """No display is connected on the bench (PC13 = 1), so the base would not send frames: force display::connected = 1
    and after the test restore the original value and clear the indicator."""
    addr = base.sym("display::connected")
    orig = base.read(addr, 1)
    try:
        base.write(addr, b"\x01")
        quiet.send_msg(MT.NOW_PLAYING_INFO, b"")  # empty payload = blank everything
        time.sleep(0.7)
        yield addr
    finally:
        try:
            quiet.send_msg(MT.NOW_PLAYING_INFO, b"")
            time.sleep(0.2)
        finally:
            base.write(addr, orig)  # restored always, even if sending fails


def npi(link, *fields):
    return link.send_msg(MT.NOW_PLAYING_INFO, bytes(fields))


def capture_while(link, base, addr, action, seconds=4.0, arm=1.5):
    """Starts a D0/D1 capture, after `arm` s performs action(), returns (frames, warnings, all bytes with times)."""
    proc, out = la.start(seconds, f"{la.CH['disp_rx']},{la.CH['disp_tx']}", f"{SAMPLERATE // 1000}k")
    time.sleep(arm)  # fx2lafw becomes active only some time after sigrok-cli starts
    try:
        action()
    finally:
        la.finish(proc, out, seconds + 30)
    assert base.read(addr, 1) == b"\x01", "display::connected se zmenil behem testu (EXTI hrana na PC13?)"
    data, warnings = la.uart_timed(out, la.CH["disp_tx"], BAUD, SAMPLERATE)
    frames = split_frames(data)
    if frames and len(frames[-1][1]) < 9 and frames[-1][0] > seconds - 0.1:
        frames.pop()  # the end of the capture truncated the last frame (in the middle of the capture it would be an error and check_clean would catch it)
    # warnings at the capture edges (start in the middle of a byte, cut at the end) are not a base error
    first = frames[0][0] if frames else 0
    warnings = [w for w in warnings if first <= w[0] <= seconds - 0.1]
    return frames, warnings, data


def check_clean(frames, warnings):
    assert warnings == [], f"chyby dekodovani UARTu: {warnings}"
    bad = [(t, f.hex()) for t, f in frames if len(f) != 9 or f[0] != 0xB0]
    assert bad == [], f"neuplne nebo spatne ramce: {bad}"


def check_period(frames, tail):
    """The last `tail` frames are identical and arrive every 500 ms."""
    last = frames[-tail:]
    assert len(last) == tail
    assert len({f for _, f in last}) == 1
    for (t0, _), (t1, _) in zip(last, last[1:]):
        assert t1 - t0 == pytest.approx(0.5, abs=0.06), f"perioda ramcu {t1 - t0:.3f} s"


def run_send(link, base, addr, *payload, prev=BLANK_FRAME):
    """Send one message during the capture; verify silence (no ACK), a clean recording, the transition prev -> expected frame."""
    exp = expected_frame(*payload, prev=prev)
    silence = []

    def action():
        link.drain()
        npi(link, *payload)
        silence.extend(link.expect_silence(0.5))

    frames, warnings, _ = capture_while(link, base, addr, action)
    assert silence == [], f"NOW_PLAYING_INFO se nema potvrzovat, prisel: {silence}"
    check_clean(frames, warnings)
    seq = [f for _, f in frames]
    assert seq[-1] == exp, f"{seq[-1].hex()} != {exp.hex()}"
    # first only the previous state, then (and from the first occurrence on only) the expected one
    first = seq.index(exp)
    assert all(f == prev for f in seq[:first]), [f.hex() for f in seq]
    assert all(f == exp for f in seq[first:]), [f.hex() for f in seq]
    assert len(seq) - first >= 3
    check_period(frames, 3)
    return exp


def settle(link, *payload):
    npi(link, *payload)
    time.sleep(0.7)


def test_now_playing_frame(quiet, base, display_on):
    exp = run_send(quiet, base, display_on, 0xF4, 0x06, 0xF1, 0x0A, 0x02)
    assert exp == bytes([0xB0, 1, BLANK, 6, 0, 4, BLANK, 0x0A, GREEN])  # " 406", verse " 1", A, green


def test_no_change_field(quiet, base, display_on):
    settle(quiet, 0xF4, 0x06, 0xF1, 0x0A, 0x02)
    prev = expected_frame(0xF4, 0x06, 0xF1, 0x0A, 0x02)
    exp = run_send(quiet, base, display_on, 0xFF, 0xFF, 0xF2, 0xFF, 0xFF, prev=prev)
    assert exp == bytes([0xB0, 2, BLANK, 6, 0, 4, BLANK, 0x0A, GREEN])


def test_blank_fields(quiet, base, display_on):
    settle(quiet, 0xF4, 0x06, 0xF1, 0x0A, 0x02)
    prev = expected_frame(0xF4, 0x06, 0xF1, 0x0A, 0x02)
    exp = run_send(quiet, base, display_on, 0xFE, 0x00, 0xFE, 0x0F, 0x00, prev=prev)
    assert exp == bytes([0xB0, BLANK, BLANK, BLANK, BLANK, BLANK, BLANK, BLANK, BLANK])


def test_digit_shift(quiet, base, display_on):
    exp = run_send(quiet, base, display_on, 0x51, 0x3F, 0xFF, 0xFF, 0xFF)
    assert exp[3:7] == bytes([BLANK, 3, 1, 5])  # "513 ": the digits stay in their positions


def test_display_burst_last_wins(quiet, base, display_on):
    states = [(0x11, 0x11, 0x11, 0x0A, 0x01), (0x22, 0x22, 0x22, 0x0B, 0x02), (0x33, 0x33, 0x33, 0x0C, 0x03),
              (0x44, 0x44, 0x44, 0x0D, 0x04), (0xF4, 0x06, 0xF1, 0x0A, 0x02)]

    def action():
        for s in states:
            npi(quiet, *s)
        time.sleep(0.6)

    frames, warnings, _ = capture_while(quiet, base, display_on, action)
    check_clean(frames, warnings)
    assert frames[-1][1] == expected_frame(*states[-1])
    check_period(frames, 3)
    seen = {f for _, f in frames}
    allowed = {BLANK_FRAME} | {expected_frame(*s) for s in states}
    assert seen <= allowed, [f.hex() for f in seen - allowed]
