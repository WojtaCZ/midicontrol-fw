#include <gtest/gtest.h>
#include <vector>
#include "running_status.hpp"

using running_status::Encoder;
using running_status::kRefreshMs;
using Bytes = std::vector<uint8_t>;

namespace {
	Bytes enc(Encoder &e, Bytes msg, uint32_t now = 0) {
		uint8_t out[3];
		uint8_t n = e.encode(msg.data(), static_cast<uint8_t>(msg.size()), now, out);
		return Bytes(out, out + n);
	}

	// Minimal receiver (MIDI 1.0 running status rules, channel messages only + SysEx/real-time skipping)
	struct Receiver {
		uint8_t status = 0;
		Bytes data;
		std::vector<Bytes> msgs;
		bool sysex = false;
		void feed(uint8_t b) {
			if (b >= 0xF8) return;
			if (b == 0xF0) { sysex = true; status = 0; return; }
			if (b == 0xF7) { sysex = false; status = 0; return; }
			if (b >= 0x80) { sysex = false; status = b >= 0xF0 ? 0 : b; data.clear(); return; }
			if (sysex || !status) return;
			data.push_back(b);
			uint8_t need = ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
			if (data.size() == need) {
				Bytes m{status};
				m.insert(m.end(), data.begin(), data.end());
				msgs.push_back(m);
				data.clear();
			}
		}
	};
}

TEST(RunningStatus, FirstMessageFullThenOmitted) {
	Encoder e;
	EXPECT_EQ(enc(e, {0x90, 0x3C, 0x64}), (Bytes{0x90, 0x3C, 0x64}));
	EXPECT_EQ(enc(e, {0x90, 0x40, 0x64}, 1), (Bytes{0x40, 0x64}));
	EXPECT_EQ(enc(e, {0x90, 0x43, 0x64}, 2), (Bytes{0x43, 0x64}));
}

TEST(RunningStatus, NoteOffBecomesNoteOnVelocityZero) {
	Encoder e;
	enc(e, {0x90, 0x3C, 0x64});
	EXPECT_EQ(enc(e, {0x80, 0x3C, 0x40}, 1), (Bytes{0x3C, 0x00}));
	Encoder f;
	EXPECT_EQ(enc(f, {0x83, 0x3C, 0x40}), (Bytes{0x93, 0x3C, 0x00}));
}

TEST(RunningStatus, StatusChangeSendsStatus) {
	Encoder e;
	enc(e, {0x90, 0x3C, 0x64});
	EXPECT_EQ(enc(e, {0x91, 0x3C, 0x64}), (Bytes{0x91, 0x3C, 0x64}));
	EXPECT_EQ(enc(e, {0xB1, 0x07, 0x7F}), (Bytes{0xB1, 0x07, 0x7F}));
	EXPECT_EQ(enc(e, {0xC1, 0x05}), (Bytes{0xC1, 0x05}));
	EXPECT_EQ(enc(e, {0xC1, 0x06}), (Bytes{0x06}));
}

TEST(RunningStatus, RefreshAfterInterval) {
	Encoder e;
	enc(e, {0x90, 0x3C, 0x64}, 1000);
	EXPECT_EQ(enc(e, {0x90, 0x3D, 0x64}, 1000 + kRefreshMs - 1).size(), 2u);
	EXPECT_EQ(enc(e, {0x90, 0x3E, 0x64}, 1000 + kRefreshMs), (Bytes{0x90, 0x3E, 0x64}));
	EXPECT_EQ(enc(e, {0x90, 0x3F, 0x64}, 1000 + kRefreshMs + 1).size(), 2u);
}

TEST(RunningStatus, RefreshAcrossClockWrap) {
	Encoder e;
	enc(e, {0x90, 0x3C, 0x64}, 0xFFFFFF00u);
	EXPECT_EQ(enc(e, {0x90, 0x3D, 0x64}, 0xFFFFFF10u).size(), 2u);
	EXPECT_EQ(enc(e, {0x90, 0x3E, 0x64}, 0xFFFFFF00u + kRefreshMs).size(), 3u);
}

TEST(RunningStatus, SysexCancels) {
	Encoder e;
	enc(e, {0x90, 0x3C, 0x64});
	EXPECT_EQ(enc(e, {0xF0, 0x4D, 0x43}), (Bytes{0xF0, 0x4D, 0x43})); // CIN 4 start
	EXPECT_EQ(enc(e, {0x01, 0x02, 0x03}), (Bytes{0x01, 0x02, 0x03})); // CIN 4 continuation (data first)
	EXPECT_EQ(enc(e, {0x04, 0xF7}), (Bytes{0x04, 0xF7}));             // CIN 6 end
	EXPECT_EQ(enc(e, {0x90, 0x3C, 0x00}), (Bytes{0x90, 0x3C, 0x00}));
}

TEST(RunningStatus, SystemCommonCancelsRealTimeKeeps) {
	Encoder e;
	enc(e, {0x90, 0x3C, 0x64});
	EXPECT_EQ(enc(e, {0xF8}), (Bytes{0xF8}));
	EXPECT_EQ(enc(e, {0x90, 0x3D, 0x64}).size(), 2u);
	EXPECT_EQ(enc(e, {0xF3, 0x01}), (Bytes{0xF3, 0x01}));
	EXPECT_EQ(enc(e, {0x90, 0x3E, 0x64}).size(), 3u);
}

TEST(RunningStatus, ResetForcesStatus) {
	Encoder e;
	enc(e, {0xB0, 0x7B, 0x00});
	e.reset();
	EXPECT_EQ(enc(e, {0xB0, 0x78, 0x00}).size(), 3u);
}

TEST(RunningStatus, DisabledPassesThrough) {
	Encoder e(false);
	EXPECT_EQ(enc(e, {0x90, 0x3C, 0x64}), (Bytes{0x90, 0x3C, 0x64}));
	EXPECT_EQ(enc(e, {0x90, 0x40, 0x64}), (Bytes{0x90, 0x40, 0x64}));
	EXPECT_EQ(enc(e, {0x80, 0x3C, 0x40}), (Bytes{0x80, 0x3C, 0x40}));
}

TEST(RunningStatus, ReceiverDecodesSameStream) {
	// Hymn-like stream: note on/off on channel 0, a CC, a program change, the display SysEx, a clock byte
	std::vector<Bytes> in = {{0x90, 60, 100}, {0x90, 64, 100}, {0x90, 67, 100}, {0x80, 60, 64}, {0x80, 64, 64}, {0x90, 62, 90},
	                         {0xB0, 7, 100},  {0xB0, 11, 90},  {0xC0, 5},       {0xF8},          {0x90, 70, 1},   {0x80, 70, 0}};
	std::vector<Bytes> expect;
	for (auto m : in) {
		if (m[0] >= 0xF0) continue;
		if ((m[0] & 0xF0) == 0x80) m = {static_cast<uint8_t>(0x90 | (m[0] & 0x0F)), m[1], 0};
		expect.push_back(m);
	}
	Encoder e;
	Receiver r;
	size_t wire = 0, full = 0;
	uint32_t t = 0;
	for (auto &m : in) {
		for (uint8_t b : enc(e, m, t += 5)) { r.feed(b); ++wire; }
		full += m.size();
	}
	EXPECT_EQ(r.msgs, expect);
	EXPECT_LT(wire, full);
}
