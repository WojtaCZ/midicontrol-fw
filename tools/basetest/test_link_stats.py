"""Link quality metrics (RNBD350 review proposals 1-5): RSSI, connection parameters, PHY and link counters in INFO_RESP of both nodes.
Precondition: base and remote are paired and linked. The values are read through the PC protocol port (GET_INFO), the path the PC status page uses.

What the bench can show: the settings were applied (parameters, PHY) and the readings are plausible. The real range has to be
checked in the church."""
import struct

import pytest

import proto
from conftest import remote_alive, wait_until
from proto import Packet

pytestmark = pytest.mark.hw

GET_INFO, INFO_RESP = 0x52, 0x53
TAG_RSSI, TAG_PARAMS, TAG_COUNTERS = 0x14, 0x15, 0x17
TAG_BATTERY_MV, TAG_VBUS_MV = 0x30, 0x33
UNKNOWN = -128
PHY_CODED = 3
INTERVAL_20MS = 16  # 1.25 ms units
TIMEOUT_2_5S = 250  # 10 ms units


def tlv(payload: bytes) -> dict:
    out, i = {}, 0
    while i + 2 <= len(payload):
        tag, n = payload[i], payload[i + 1]
        out[tag] = payload[i + 2:i + 2 + n]
        i += 2 + n
    return out


def info(link, dst):
    link.send_msg(GET_INFO, dst=dst)
    pkt = link.recv(4, lambda p: p.mtype == INFO_RESP and p.src == dst)
    assert pkt is not None, f"no INFO_RESP from {dst}"
    # INFO_RESP is a reliable message: without our ACK the node retransmits it 3 times, and the copies land in the next test
    link.send(Packet(proto.ADDR_PC, dst, 0, proto.MT.ACK, bytes([pkt.pid])))
    return tlv(pkt.payload)


@pytest.fixture(scope="module")
def linked(link, base):
    wait_until(lambda: remote_alive(base), 30, interval=0.5, what="remote alive")
    # the first RSSI poll comes within ~10 s of the secured link
    wait_until(lambda: struct.unpack("2b", info(link, proto.ADDR_BASE).get(TAG_RSSI, b"\x80\x80"))[0] != UNKNOWN, 25, interval=1.0, what="base RSSI")


@pytest.mark.parametrize("dst", [proto.ADDR_BASE, proto.ADDR_REMOTE])
def test_link_settings_are_applied(link, linked, dst):
    t = info(link, dst)
    interval, timeout, latency, phy = struct.unpack("<HHBB", t[TAG_PARAMS])
    assert interval == INTERVAL_20MS and latency == 0, "no latency: it would delay notes"
    assert timeout == TIMEOUT_2_5S, f"supervision timeout {timeout * 10} ms (module default 720 ms means the tuning was not applied)"
    assert phy == (PHY_CODED << 4 | PHY_CODED), f"Coded PHY expected on the link, got {phy:#x}"


@pytest.mark.parametrize("dst", [proto.ADDR_BASE, proto.ADDR_REMOTE])
def test_rssi_is_plausible(link, linked, dst):
    t = info(link, dst)
    rssi = struct.unpack("2b", t[TAG_RSSI])[0]
    assert -100 < rssi < 0, rssi


def test_remote_rssi_reaches_the_base_and_back(link, linked):
    # the remote's RSSI rides in KEEPALIVE_ACK, the base's in KEEPALIVE: each node knows what the other measured
    def both():
        b = struct.unpack("2b", info(link, proto.ADDR_BASE).get(TAG_RSSI, b"\x80\x80"))[1]
        r = struct.unpack("2b", info(link, proto.ADDR_REMOTE).get(TAG_RSSI, b"\x80\x80"))[1]
        return b != UNKNOWN and r != UNKNOWN
    wait_until(both, 25, interval=1.0, what="peer RSSI on both nodes")


def test_counters_have_the_documented_layout(link, linked):
    for dst in (proto.ADDR_BASE, proto.ADDR_REMOTE):
        c = info(link, dst)[TAG_COUNTERS]
        assert len(c) == 6
        losses, err_conn, reboots, retries, reconnect_ms = struct.unpack("<4BH", c)
        assert err_conn <= losses


def test_info_keeps_the_older_fields(link, linked):
    """The remote's INFO_RESP is nearly at the 128-byte payload limit: the link fields come last so that nothing older (VBUS_MV) is dropped."""
    t = info(link, proto.ADDR_REMOTE)
    assert TAG_BATTERY_MV in t and TAG_VBUS_MV in t, sorted(t)
    assert TAG_RSSI in t and TAG_PARAMS in t and TAG_COUNTERS in t, sorted(t)
