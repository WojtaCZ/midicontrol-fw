// statusframe::Framer: status messages are split out of the RX stream and a lost or extra '%' never swallows what follows
// (bench 2026-11-13: base inside a "status" for good, "CMD> " swallowed, every "$$$" timed out until an MCU reset).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "status_framer.hpp"

using statusframe::Framer;
using statusframe::Result;

namespace {
	struct Out {
		std::vector<std::string> statuses;
		std::string passed; // bytes handed back to the caller
	};

	// All bytes of `in` arrive at `t` ms (one burst)
	void feed(Framer &f, const std::string &in, uint32_t t, Out &o) {
		for (char ch : in) {
			const auto c = static_cast<unsigned char>(ch);
			switch (f.feed(c, t)) {
			case Result::COMPLETE: o.statuses.emplace_back(f.text()); break;
			case Result::NOT_STATUS: o.passed.push_back(static_cast<char>(c)); break;
			case Result::CONSUMED: break;
			}
		}
	}
}

TEST(StatusFramer, SplitsStatusesFromData) {
	Framer f;
	Out o;
	feed(f, "%REBOOT%CMD> %CONNECT,1,40843204625F%AOK\r\n", 0, o);
	ASSERT_EQ(o.statuses.size(), 2u);
	EXPECT_EQ(o.statuses[0], "REBOOT");
	EXPECT_EQ(o.statuses[1], "CONNECT,1,40843204625F");
	EXPECT_EQ(o.passed, "CMD> AOK\r\n");
	EXPECT_FALSE(f.active());
	EXPECT_EQ(f.resyncs(), 0u);
}

TEST(StatusFramer, StrayPercentThenPauseLetsCmdPromptThrough) {
	// The bench case: a stray '%' leaves the framer inside a status; "CMD> " arrives later and must reach the caller
	Framer f;
	Out o;
	feed(f, "%", 1000, o);
	feed(f, "CMD> ", 1500, o);
	EXPECT_EQ(o.passed, "CMD> ");
	EXPECT_TRUE(o.statuses.empty());
	EXPECT_EQ(f.resyncs(), 1u);
}

TEST(StatusFramer, RebootAfterStrayPercentIsStillRecognised) {
	// Old ISR: the '%' of "%REBOOT%" closed the stale status and "REBOOT" went to data. Now the pause resyncs first
	Framer f;
	Out o;
	feed(f, "%", 0, o);
	feed(f, "%REBOOT%", 3000, o);
	ASSERT_EQ(o.statuses.size(), 1u);
	EXPECT_EQ(o.statuses[0], "REBOOT");
	EXPECT_TRUE(o.passed.empty());
}

TEST(StatusFramer, ControlByteEndsStatus) {
	Framer f;
	Out o;
	feed(f, "%AOK\r\n", 0, o);
	EXPECT_TRUE(o.statuses.empty());
	EXPECT_EQ(o.passed, "\r\n");
	EXPECT_EQ(f.resyncs(), 1u);
	EXPECT_FALSE(f.active());
}

TEST(StatusFramer, FrameStartEndsStatus) {
	Framer f;
	Out o;
	feed(f, std::string("%xy\x01\x02", 5), 0, o);
	EXPECT_EQ(o.passed, std::string("\x01\x02", 2));
	EXPECT_FALSE(f.active());
}

TEST(StatusFramer, DoublePercentStaysInside) {
	Framer f;
	Out o;
	feed(f, "%%STREAM_OPEN%", 0, o);
	ASSERT_EQ(o.statuses.size(), 1u);
	EXPECT_EQ(o.statuses[0], "STREAM_OPEN");
}

TEST(StatusFramer, LongScanResultIsTruncatedNotAbandoned) {
	Framer f;
	Out o;
	std::string body(200, 'A');
	feed(f, "%" + body + "%", 0, o);
	ASSERT_EQ(o.statuses.size(), 1u);
	EXPECT_EQ(o.statuses[0], body.substr(0, Framer::TEXT_LEN - 1));
	EXPECT_TRUE(o.passed.empty());
	EXPECT_EQ(f.resyncs(), 0u);
}

TEST(StatusFramer, GapExactlyAtLimitDoesNotResync) {
	Framer f;
	Out o;
	feed(f, "%ST", 100, o);
	feed(f, "OP%", 100 + Framer::GAP_MS, o);
	ASSERT_EQ(o.statuses.size(), 1u);
	EXPECT_EQ(o.statuses[0], "STOP");
}

TEST(StatusFramer, ResetLeavesStatus) {
	Framer f;
	Out o;
	feed(f, "%ABC", 0, o);
	f.reset();
	feed(f, "CMD> ", 0, o);
	EXPECT_EQ(o.passed, "CMD> ");
}

TEST(StatusFramer, WrapsAroundTimeCounter) {
	Framer f;
	Out o;
	feed(f, "%ST", 0xFFFFFFF0u, o);
	feed(f, "OP%", 0x00000010u, o); // 32 ms later across the wrap
	ASSERT_EQ(o.statuses.size(), 1u);
	EXPECT_EQ(o.statuses[0], "STOP");
}
