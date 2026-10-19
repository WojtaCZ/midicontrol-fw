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


def test_diag_resp_roundtrip_8_records():
    from proto import DIAG_CODES, DIAG_RECORD_SIZE, diag_decode_resp, diag_encode_record, diag_encode_resp
    assert MT.DIAG_GET == 0x80 and MT.DIAG_RESP == 0x81 and DIAG_RECORD_SIZE == 12
    assert sorted(DIAG_CODES) == list(range(1, 17))
    assert len(diag_encode_record(0, 1, 1, 0, 0)) == 12
    for batch in (range(1, 9), range(9, 17)):  # together: every code
        recs = [(seq, code, 1 + seq % 4, (0x01020304 * (seq + 1)) & 0xFFFFFFFF, 0xDEADBEEF)
                for seq, code in enumerate(batch)]
        payload = diag_encode_resp(0x1B24, 8, recs)  # boot_id contains escaped bytes (0x1B, 0x24)
        assert len(payload) == 5 + 8 * 12 == 101
        frame = encode(3, 1, 9, MT.DIAG_RESP, payload)
        p = FrameParser()
        got = [pkt for b in frame for pkt in p.feed(bytes([b]))]
        assert len(got) == 1 and got[0].mtype == MT.DIAG_RESP and got[0].payload == payload
        assert diag_decode_resp(got[0].payload) == (0x1B24, 8, recs)


def test_diag_records_with_escaped_bytes_survive():
    from proto import diag_decode_resp, diag_encode_resp
    # record bytes equal to SOH/EOT/ESC/$/% inside seq, uptime and arg
    recs = [(0x0104, 16, 4, 0x25240104, 0x1B1B2425)] * 8
    payload = diag_encode_resp(0x0401, 0x2524, recs)
    frame = encode(3, 1, 1, MT.DIAG_RESP, payload)
    p = FrameParser()
    got = [pkt for b in frame for pkt in p.feed(bytes([b]))]
    assert len(got) == 1 and diag_decode_resp(got[0].payload) == (0x0401, 0x2524, recs)
    assert p.bad_frames == 0 and p.crc_errors == 0


def test_diag_get_payload():
    from proto import diag_encode_get
    assert diag_encode_get(0x0102) == b"\x02\x01"
