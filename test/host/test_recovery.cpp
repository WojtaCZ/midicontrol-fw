// Host tests for logic/recovery: the DIN All Notes Off cleanup sent at every boot.
#include <gtest/gtest.h>
#include "recovery.hpp"

using namespace recovery;

TEST(Recovery, MessageSequence) {
	// All Sound Off (120) and All Notes Off (123) on each of the 16 channels, in channel order
	uint8_t m[3];
	ASSERT_EQ(cleanupMessageCount(), 32u);
	cleanupMessage(0, m);  EXPECT_EQ(m[0], 0xB0); EXPECT_EQ(m[1], 123); EXPECT_EQ(m[2], 0);
	cleanupMessage(1, m);  EXPECT_EQ(m[0], 0xB0); EXPECT_EQ(m[1], 120);
	cleanupMessage(31, m); EXPECT_EQ(m[0], 0xBF); EXPECT_EQ(m[1], 120);
	cleanupMessage(30, m); EXPECT_EQ(m[0], 0xBF); EXPECT_EQ(m[1], 123);
}

// SYS-PC-03 / D1 / D19: cut-off only on the transition PC present -> PC lost
TEST(PcLossGuard, NeverFiresWithoutAPcEverPresent) {
	PcLossGuard g;
	for (int i = 0; i < 5; ++i) EXPECT_FALSE(g.update(false));
}

TEST(PcLossGuard, FiresOnceOnLoss) {
	PcLossGuard g;
	EXPECT_FALSE(g.update(true));
	EXPECT_FALSE(g.update(true));
	EXPECT_TRUE(g.update(false));
	EXPECT_FALSE(g.update(false));
	EXPECT_FALSE(g.update(false));
}

TEST(PcLossGuard, RearmsWhenThePcReturns) {
	PcLossGuard g;
	g.update(true);
	EXPECT_TRUE(g.update(false));
	EXPECT_FALSE(g.update(true));
	EXPECT_TRUE(g.update(false));
}
