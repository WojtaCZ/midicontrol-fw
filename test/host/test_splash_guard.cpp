// P1-8: splash auto-dismiss vs. a real key first.
#include <gtest/gtest.h>

#include "splash_guard.hpp"

using splashguard::Guard;

TEST(SplashGuard, TimerFiresWhenNoKeyCame) {
	Guard g;
	EXPECT_TRUE(g.allowAutoDismiss());
}

TEST(SplashGuard, RealKeyBeforeTimerSuppressesIt) {
	Guard g;
	g.realKey();
	EXPECT_FALSE(g.allowAutoDismiss());
}

TEST(SplashGuard, SuppressionStaysAfterMoreKeys) {
	Guard g;
	g.realKey();
	g.realKey();
	EXPECT_FALSE(g.allowAutoDismiss());
}
