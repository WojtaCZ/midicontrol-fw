"""Offline tests of dbg.Target without a probe: access width when reading numbers (ruling R36).

The 600 s soak of 2026-10-24 reported "base systick went back (271871 -> 271819)": u() assembled a 4 B value from byte-wise reads
(`read_memory addr 8 4`), which got torn across the transfer while an ISR was incrementing ticks_. Aligned 2/4 B values are
therefore read with a single 16/32-bit access."""

import dbg


class FakeTarget(dbg.Target):
    """Target without openocd: tcl() only records the command and returns an answer from the memory `mem` (little-endian bytes)."""

    def __init__(self, mem: dict, syms: dict):
        self.name = "fake"
        self._sym = dict(syms)
        self.mem = mem
        self.cmds = []

    def tcl(self, cmd: str) -> str:
        self.cmds.append(cmd)
        _, addr, width, count = cmd.split()
        addr, width, count = int(addr, 0), int(width), int(count)
        step = width // 8
        vals = [int.from_bytes(bytes(self.mem.get(addr + i * step + j, 0) for j in range(step)), "little") for i in range(count)]
        return " ".join(f"0x{v:x}" for v in vals)


def mem_u32(addr, value):
    return {addr + i: (value >> (8 * i)) & 0xFF for i in range(4)}


def test_aligned_u32_is_one_32bit_access():
    t = FakeTarget(mem_u32(0x20000100, 0x000425FF), {"stmcpp::systick::ticks_": 0x20000100})
    assert t.u("stmcpp::systick::ticks_") == 0x000425FF
    assert t.cmds == ["read_memory 0x20000100 32 1"]


def test_aligned_u16_is_one_16bit_access():
    t = FakeTarget({0x20000102: 0x34, 0x20000103: 0x12}, {"x": 0x20000102})
    assert t.u("x", 2) == 0x1234
    assert t.cmds == ["read_memory 0x20000102 16 1"]


def test_u8_and_unaligned_stay_bytewise():
    t = FakeTarget({0x20000101: 0xAB, **mem_u32(0x20000105, 0x11223344)}, {"b": 0x20000101, "odd": 0x20000105})
    assert t.read_u8("b") == 0xAB
    assert t.u("odd") == 0x11223344
    assert t.cmds == ["read_memory 0x20000101 8 1", "read_memory 0x20000105 8 4"]
