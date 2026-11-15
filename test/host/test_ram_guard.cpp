// Host tests for logic/ram_guard (TL-1): heap break limit and the stack high-water scan.
#include <gtest/gtest.h>
#include "ram_guard.hpp"

#include <cstdint>

using namespace ramguard;

TEST(RamGuard, BreakMayGrowUpToTheLimit) {
	EXPECT_TRUE(breakAllowed(0x1000, 0x100, 0x1100, 0x1000));
	EXPECT_TRUE(breakAllowed(0x1000, 0, 0x1100, 0x1000));
}

TEST(RamGuard, BreakPastTheLimitFails) {
	EXPECT_FALSE(breakAllowed(0x1000, 0x101, 0x1100, 0x1000));
	EXPECT_FALSE(breakAllowed(0x1200, 4, 0x1100, 0x1000)); // already past (must not happen, still refused)
}

TEST(RamGuard, HugeIncrementDoesNotWrapAround) {
	EXPECT_FALSE(breakAllowed(0x20006000, 0x7FFFFFFF, 0x20007400, 0x20006000));
}

TEST(RamGuard, NegativeIncrementShrinksButNotBelowTheStart) {
	EXPECT_TRUE(breakAllowed(0x1100, -0x100, 0x1100, 0x1000));
	EXPECT_FALSE(breakAllowed(0x1100, -0x101, 0x1100, 0x1000));
}

TEST(RamGuard, UntouchedStackIsCountedFromTheLowEnd) {
	uint32_t area[16];
	for (auto &w : area) w = PAINT;
	EXPECT_EQ(untouchedBytes(area, area + 16), 64u);
	area[10] = 0x12345678; // deepest stack write reached word 10
	EXPECT_EQ(untouchedBytes(area, area + 16), 40u);
	area[0] = 0; // a heap write at the low end ends the scan at once
	EXPECT_EQ(untouchedBytes(area, area + 16), 0u);
}

TEST(RamGuard, EmptyOrInvertedRangeIsZero) {
	uint32_t area[2] = {PAINT, PAINT};
	EXPECT_EQ(untouchedBytes(area, area), 0u);
	EXPECT_EQ(untouchedBytes(area + 2, area), 0u);
}
