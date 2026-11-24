#include <gtest/gtest.h>
#include <ui/auto_close.hpp>
#include <ui/link_error.hpp>

using namespace ui;

TEST(AutoClose, ClosesOnceAtTimeout) {
	AutoClose c;
	c.start(5000);
	int fired = 0;
	for (int ms = 30; ms <= 8000; ms += 30)
		if (c.tick(30)) { fired++; EXPECT_GE(ms, 5000); EXPECT_LT(ms, 5030); }
	EXPECT_EQ(fired, 1);
	EXPECT_FALSE(c.active());
}

TEST(AutoClose, KeyClosesOnceAndStopsTimer) {
	AutoClose c;
	c.start(5000);
	EXPECT_TRUE(c.key());
	EXPECT_FALSE(c.key());
	EXPECT_FALSE(c.tick(10000));
}

TEST(AutoClose, KeyAfterTimeoutDoesNothing) {
	AutoClose c;
	c.start(100);
	EXPECT_TRUE(c.tick(100));
	EXPECT_FALSE(c.key());
}

TEST(AutoClose, NotArmedNeverFires) {
	AutoClose c;
	EXPECT_FALSE(c.tick(100000));
	EXPECT_FALSE(c.key());
}

TEST(AutoClose, StartRearms) {
	AutoClose c;
	c.start(100);
	EXPECT_TRUE(c.tick(100));
	c.start(100);
	EXPECT_FALSE(c.tick(99));
	EXPECT_TRUE(c.tick(1));
}

TEST(LinkError, NoLinkIsConnLostAtOnce) {
	LinkErrorPolicy p;
	EXPECT_EQ(p.classify(LinkFailure::NoLink, true, false, 0), LinkVerdict::ConnLost);
}

TEST(LinkError, SingleTimeoutIsNoResponse) {
	LinkErrorPolicy p;
	EXPECT_EQ(p.classify(LinkFailure::Timeout, true, false, 1000), LinkVerdict::NoResponse);
}

TEST(LinkError, SecondTimeoutWithinWindowIsConnLost) {
	LinkErrorPolicy p;
	EXPECT_EQ(p.classify(LinkFailure::Timeout, true, false, 1000), LinkVerdict::NoResponse);
	EXPECT_EQ(p.classify(LinkFailure::Timeout, true, false, 10999), LinkVerdict::ConnLost);
}

TEST(LinkError, SecondTimeoutAfterWindowIsNoResponseAgain) {
	LinkErrorPolicy p;
	p.classify(LinkFailure::Timeout, true, false, 1000);
	EXPECT_EQ(p.classify(LinkFailure::Timeout, true, false, 11000), LinkVerdict::NoResponse);
}

TEST(LinkError, PeerDeadOrStopTimeoutIsConnLost) {
	LinkErrorPolicy p;
	EXPECT_EQ(p.classify(LinkFailure::Timeout, false, false, 0), LinkVerdict::ConnLost);
	LinkErrorPolicy q;
	EXPECT_EQ(q.classify(LinkFailure::Timeout, true, true, 0), LinkVerdict::ConnLost);
}

TEST(LinkError, NakIsErrorAndDoesNotCountAsTimeout) {
	LinkErrorPolicy p;
	EXPECT_EQ(p.classify(LinkFailure::Nak, true, false, 0), LinkVerdict::Error);
	EXPECT_EQ(p.classify(LinkFailure::Timeout, true, false, 100), LinkVerdict::NoResponse);
}

TEST(LinkError, ClockWrapIsHandled) {
	LinkErrorPolicy p;
	p.classify(LinkFailure::Timeout, true, false, 0xFFFFFF00u);
	EXPECT_EQ(p.classify(LinkFailure::Timeout, true, false, 0x100u), LinkVerdict::ConnLost);
}
