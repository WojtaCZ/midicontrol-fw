"""Output to the church display: NOW_PLAYING_INFO over USB-CDC -> a 9-byte frame on the display UART (1200 Bd).

No logic analyzer (owner 2026-11-22): the frames are observed from the MCU. display::startFrame() copies currentState to txBuffer and
sets txBusy for the 75 ms the TX DMA runs; TxTap polls txBusy over gdb and records txBuffer at every rising edge = "a frame went out".
(Two frames back to back may merge into one edge; the checks tolerate it.)

Also the receive side and bus sharing (the bus is multi-master; there is no second master on the bench, so foreign
traffic is simulated through the probe):
- test_rx_frame_flag_check (SYS-BUS-08, D26, gap 24): a received frame that does not start with 0xB0 is dropped and
  leaves the state alone; a valid one is mirrored.
- test_rx_off_for_the_whole_frame (HARDWARE.md note 6): the RX DMA stays off until the last stop bit is on the wire
  (USART TC), not just until the last byte enters TDR, so the echo of our own frame never reaches the RX buffer.
- test_quiet_bus_holds_tx (SYS-BUS-05, D25, gap 29): while the bus shows activity the base does not transmit; it
  sends once the bus has been quiet.
"""

import time

import pytest

import proto
from proto import MT

ADDR_BASE = proto.ADDR_BASE

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


class TxTap:
    """Records the display frames the base starts (txBusy rising edge -> txBuffer), over gdb (~2-3 ms per step)."""

    def __init__(self, base):
        self.base = base
        self.busy_addr, self.buf_addr = base.sym("display::txBusy"), base.sym("display::txBuffer")
        self.frames, self.prev = [], 0
        self.t0 = time.monotonic()

    def step(self):
        busy = self.base.read_u8("display::txBusy")
        if busy and not self.prev:
            self.frames.append((time.monotonic() - self.t0, self.base.read(self.buf_addr, 9)))
        self.prev = busy

    def sleep(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.step()


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
    """Starts following the TX frames, after `arm` s performs action(tap), keeps following until `seconds` in total.
    Returns the frames [(time, 9 bytes)]. The action must use tap.sleep() / tap.step() instead of time.sleep()."""
    tap = TxTap(base)
    tap.sleep(arm)
    action(tap)
    tap.sleep(max(0.0, seconds - (time.monotonic() - tap.t0)))
    assert base.read(addr, 1) == b"\x01", "display::connected changed during the test (EXTI edge on PC13?)"
    return tap.frames


def check_clean(frames):
    bad = [(t, f.hex()) for t, f in frames if len(f) != 9 or f[0] != 0xB0]
    assert bad == [], f"incomplete or wrong frames: {bad}"


def check_period(frames, tail):
    """The last `tail` frames are identical and arrive every 500 ms (polling jitter of a few ms)."""
    last = frames[-tail:]
    assert len(last) == tail
    assert len({f for _, f in last}) == 1
    for (t0, _), (t1, _) in zip(last, last[1:]):
        assert t1 - t0 == pytest.approx(0.5, abs=0.08), f"frame period {t1 - t0:.3f} s"


def run_send(link, base, addr, *payload, prev=BLANK_FRAME):
    """Send one message during the capture; verify silence (no ACK), a clean recording, the transition prev -> expected frame."""
    exp = expected_frame(*payload, prev=prev)
    silence = []

    def action(tap):
        link.drain()
        npi(link, *payload)
        tap.sleep(0.5)
        silence.extend(link.drain())

    frames = capture_while(link, base, addr, action)
    assert silence == [], f"NOW_PLAYING_INFO must not be acknowledged, got: {silence}"
    check_clean(frames)
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

    def action(tap):
        for s in states:
            npi(quiet, *s)
        tap.sleep(0.6)

    frames = capture_while(quiet, base, display_on, action)
    check_clean(frames)
    assert frames[-1][1] == expected_frame(*states[-1])
    check_period(frames, 3)
    seen = {f for _, f in frames}
    allowed = {BLANK_FRAME} | {expected_frame(*s) for s in states}
    assert seen <= allowed, [f.hex() for f in seen - allowed]


# --- receive side and bus sharing -------------------------------------------------------------------------------

DMA2_CH3_CCR, DMA2_CH3_CNDTR = 0x40020430, 0x40020434
FRAME_S = 9 * 10 / BAUD  # 75 ms


def test_rx_frame_flag_check(quiet, base, display_on):
    settle(quiet, 0xF4, 0x06, 0xF1, 0x0A, 0x02)
    shown = expected_frame(0xF4, 0x06, 0xF1, 0x0A, 0x02)
    state, rxbuf = base.sym("display::currentState"), base.sym("display::rxBuffer")
    acc0, rej0 = base.u("display::rxAccepted"), base.u("display::rxRejected")
    assert base.read(state, 9) == shown

    for bad in (bytes([0x01, BLANK, 0x02, 0x01, BLANK, BLANK, 0x0A, RED, 0xB0]),  # caught from the 2nd byte
                bytes([0x00] * 9), bytes([0xB1, 1, 2, 3, 4, BLANK, BLANK, 0x0B, BLUE])):
        base.write(rxbuf, bad)
        base.call("display::onRxFrame()")
        assert base.read(state, 9) == shown, f"frame {bad.hex()} changed the state"
    assert base.u("display::rxRejected") - rej0 == 3
    assert base.u("display::rxAccepted") == acc0

    foreign = bytes([0xB0, 2, BLANK, 2, 1, BLANK, BLANK, 0x0B, YELLOW])  # another master: song 12, verse 2, B, yellow
    base.write(rxbuf, foreign)
    base.call("display::onRxFrame()")
    assert base.read(state, 9) == foreign
    assert base.u("display::rxAccepted") == acc0 + 1


def test_rx_off_for_the_whole_frame(quiet, base, display_on):
    """Polls the RX DMA enable bit through the probe (~1 ms per read) across a few 500 ms refresh frames."""
    t0, prev, off_at, windows = time.monotonic(), None, None, []
    while time.monotonic() - t0 < 2.5:
        en = base.read_uint(DMA2_CH3_CCR) & 1
        now = time.monotonic()
        if prev == 1 and en == 0:
            off_at = now
        elif prev == 0 and en == 1 and off_at is not None:
            windows.append(now - off_at)
        prev = en
    print(f"RX DMA off windows (ms): {[round(w * 1000) for w in windows]}")
    assert len(windows) >= 3, windows
    # the frame is 75 ms on the wire; before the fix RX came back after ~58 ms (DMA TC), in the middle of byte 8
    assert min(windows) >= FRAME_S - 0.002, f"RX re-enabled before the frame left the wire: {windows}"
    assert max(windows) < FRAME_S + 0.03, windows
    assert base.read_uint(DMA2_CH3_CNDTR) == 9


def test_quiet_bus_holds_tx(quiet, base, display_on):
    """Keeps reporting bus activity (rxActivityMs/Count, as the RX ISRs do) for HOLD_S: no frame may start then, even
    though the state changed and the 500 ms refresh is due; afterwards the new state goes out."""
    HOLD_S = 1.2
    ticks, act_ms, act_n = base.sym("stmcpp::systick::ticks_"), base.sym("display::rxActivityMs"), base.sym("display::rxActivityCount")
    changed = {}

    def action(tap):
        n = base.read_uint(act_n)
        t0 = time.monotonic()
        npi(quiet, 0x12, 0x34, 0xF5, 0x0C, 0x03)
        while time.monotonic() - t0 < HOLD_S:
            n += 1
            base.write32(act_ms, base.read_uint(ticks))
            base.write32(act_n, n & 0xFFFFFFFF)
            tap.step()
            if "mid" not in changed and time.monotonic() - t0 > HOLD_S / 2:
                changed["mid"] = base.read_bool("display::changed")
        tap.sleep(0.8)

    frames = capture_while(quiet, base, display_on, action, seconds=5.0)
    check_clean(frames)
    starts = [t for t, _ in frames]
    gaps = [b - a for a, b in zip(starts, starts[1:])]
    print(f"frame starts {[round(t, 3) for t in starts]}, longest gap {max(gaps):.3f} s, changed mid-hold {changed}")
    assert changed.get("mid") is True, "the new state should be pending (not sent) during the hold"
    assert max(gaps) >= HOLD_S - 0.15, f"a frame went out while the bus was busy: gaps {gaps}"
    assert frames[-1][1] == expected_frame(0x12, 0x34, 0xF5, 0x0C, 0x03)
