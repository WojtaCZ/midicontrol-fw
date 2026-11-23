#include <gtest/gtest.h>
#include "hold_timer.hpp"

using namespace holdtimer;

TEST(HoldTimer, FiresOnceAtThreeSeconds) {
	Timer t;
	int fired = 0;
	for (int ms = 10; ms <= 5000; ms += 10) {
		if (t.tick(true, 10)) {
			fired++;
			EXPECT_EQ(ms, 3000);
		}
	}
	EXPECT_EQ(fired, 1);
	EXPECT_EQ(t.percent(), 100);
}

TEST(HoldTimer, ReleaseStartsOver) {
	Timer t;
	for (int i = 0; i < 200; i++) EXPECT_FALSE(t.tick(true, 10));
	EXPECT_EQ(t.percent(), 66);
	EXPECT_FALSE(t.tick(false, 10));
	EXPECT_EQ(t.percent(), 0);
	for (int i = 0; i < 299; i++) EXPECT_FALSE(t.tick(true, 10));
	EXPECT_TRUE(t.tick(true, 10));
}

TEST(HoldTimer, FiresAgainAfterRelease) {
	Timer t;
	for (int i = 0; i < 300; i++) t.tick(true, 10);
	t.tick(false, 10);
	bool again = false;
	for (int i = 0; i < 300; i++) again = t.tick(true, 10) || again;
	EXPECT_TRUE(again);
}

TEST(PressTracker, ShortPressComesOnRelease) {
	PressTracker p;
	for (int i = 0; i < 10; i++) EXPECT_EQ(p.tick(true, 10), Press::None);
	EXPECT_EQ(p.tick(false, 10), Press::Short);
	EXPECT_EQ(p.tick(false, 10), Press::None);
}

TEST(PressTracker, LongPressFiresOnceAndNoShortAfterIt) {
	PressTracker p;
	int longs = 0;
	for (int i = 0; i < 200; i++) longs += p.tick(true, 10) == Press::Long;
	EXPECT_EQ(longs, 1);
	EXPECT_EQ(p.tick(false, 10), Press::None);
}

TEST(PressTracker, NoiseIsIgnored) {
	PressTracker p;
	EXPECT_EQ(p.tick(true, 10), Press::None);
	EXPECT_EQ(p.tick(false, 10), Press::None);
}

TEST(PressTracker, ButtonDownAtPowerUpNeedsARelease) {
	PressTracker p;
	p.ignoreUntilRelease();
	for (int i = 0; i < 200; i++) EXPECT_EQ(p.tick(true, 10), Press::None);
	EXPECT_EQ(p.tick(false, 10), Press::None);
	for (int i = 0; i < 10; i++) p.tick(true, 10);
	EXPECT_EQ(p.tick(false, 10), Press::Short);
}
