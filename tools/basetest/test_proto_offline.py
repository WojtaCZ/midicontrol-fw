from proto import crc8, encode, FrameParser, MT

def test_crc_check_value():
    assert crc8(b"123456789") == 0xA2

def test_roundtrip_lengths_bytewise():
    for n in (0, 1, 127, 128):
        payload = bytes((i * 37) & 0xFF for i in range(n))
        frame = encode(1, 2, 7, MT.SIGNAL_ANCHOR, payload)
        p = FrameParser()
        got = [pkt for b in frame for pkt in p.feed(bytes([b]))]
        assert len(got) == 1 and got[0].payload == payload

def test_bad_crc_dropped():
    frame = bytearray(encode(1, 2, 1, MT.KEEPALIVE))
    frame[-2] ^= 0x55
    p = FrameParser()
    assert p.feed(bytes(frame)) == [] and p.crc_errors == 1
