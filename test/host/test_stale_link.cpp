#include <gtest/gtest.h>
#include "stale_link.hpp"

using stalelink::Action;
using stalelink::Guard;

namespace {
	// Run the guard every 10 ms from `from` to `to` (exclusive); return the first non-NONE action and its time.
	struct Hit {
		Action a = Action::NONE;
		uint32_t at = 0;
	};
	Hit run(Guard &g, bool active, bool rxSeen, uint32_t lastRx, uint32_t from, uint32_t to) {
		for (uint32_t t = from; t != to; t += 10) {
			Action a = g.update(active, rxSeen, lastRx, t);
			if (a != Action::NONE) return {a, t};
		}
		return {};
	}
} // namespace

TEST(StaleLink, InactiveNeverActs) {
	Guard g;
	EXPECT_EQ(run(g, false, false, 0, 0, 60000).a, Action::NONE);
}

TEST(StaleLink, KeepaliveTrafficKeepsLinkQuiet) {
	Guard g;
	for (uint32_t t = 0; t < 120000; t += 10) {
		uint32_t lastRx = t - (t % 2000); // a frame every 2 s
		ASSERT_EQ(g.update(true, true, lastRx, t), Action::NONE) << t;
	}
}

TEST(StaleLink, SilentLinkKicksAfterStaleThenRestartsOnce) {
	Guard g;
	g.update(true, true, 500, 1000); // accepted at 1000, last frame from an earlier session
	Hit k = run(g, true, true, 500, 1010, 30000);
	ASSERT_EQ(k.a, Action::KICK);
	EXPECT_EQ(k.at, 1000 + Guard::STALE_MS);
	Hit r = run(g, true, true, 500, k.at + 10, k.at + 20000);
	ASSERT_EQ(r.a, Action::RESTART);
	EXPECT_EQ(r.at, k.at + Guard::RESTART_AFTER_MS);
	EXPECT_EQ(run(g, true, true, 500, r.at + 10, r.at + 60000).a, Action::NONE); // one restart per episode
}

TEST(StaleLink, CountsFromLastFrameWhenNewer) {
	Guard g;
	g.update(true, true, 0, 1000);
	ASSERT_EQ(run(g, true, true, 0, 1010, 4000).a, Action::NONE);
	Hit k = run(g, true, true, 4000, 4000, 30000); // a frame at 4000
	ASSERT_EQ(k.a, Action::KICK);
	EXPECT_EQ(k.at, 4000 + Guard::STALE_MS);
}

TEST(StaleLink, NeverSeenRemoteStillKicks) {
	Guard g;
	g.update(true, false, 0, 0);
	EXPECT_EQ(run(g, true, false, 0, 10, 30000).at, Guard::STALE_MS);
}

TEST(StaleLink, FrameAfterKickCancelsRestart) {
	Guard g;
	g.update(true, true, 0, 1000);
	Hit k = run(g, true, true, 0, 1010, 30000);
	ASSERT_EQ(k.a, Action::KICK);
	EXPECT_EQ(g.update(true, true, k.at + 100, k.at + 200), Action::NONE);
	// back to watching from that frame
	Hit k2 = run(g, true, true, k.at + 100, k.at + 210, k.at + 30000);
	EXPECT_EQ(k2.a, Action::KICK);
	EXPECT_EQ(k2.at, k.at + 100 + Guard::STALE_MS);
}

TEST(StaleLink, DisconnectResetsEpisode) {
	Guard g;
	g.update(true, true, 0, 1000);
	ASSERT_EQ(run(g, true, true, 0, 1010, 30000).a, Action::KICK);
	EXPECT_EQ(g.update(false, true, 0, 12000), Action::NONE);
	g.update(true, true, 0, 20000); // reconnected
	Hit k = run(g, true, true, 0, 20010, 60000);
	EXPECT_EQ(k.a, Action::KICK);
	EXPECT_EQ(k.at, 20000 + Guard::STALE_MS);
}

TEST(StaleLink, WrapSafe) {
	Guard g;
	const uint32_t start = 0xFFFFF000u;
	g.update(true, true, start - 100, start);
	Hit k = run(g, true, true, start - 100, start + 10, start + 30000);
	ASSERT_EQ(k.a, Action::KICK);
	EXPECT_EQ(k.at, start + Guard::STALE_MS);
}
