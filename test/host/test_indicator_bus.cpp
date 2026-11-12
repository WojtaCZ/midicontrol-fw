// Host tests for logic/indicator_bus: which received indicator frames the base accepts (SYS-BUS-08, D26, gap 24)
// and when it may transmit on the multi-master bus (SYS-BUS-05, D25, gap 29).
#include <gtest/gtest.h>
#include "indicator_bus.hpp"

using namespace indicator;

TEST(IndicatorFrame, NineBytesWithFlagAccepted) {
	const uint8_t f[9] = {0xB0, 0x01, 0xE0, 0x02, 0x01, 0xE0, 0xE0, 0x0A, 0x20};
	EXPECT_TRUE(frameAccepted(f, sizeof f));
}

TEST(IndicatorFrame, WrongFlagDropped) {
	uint8_t f[9] = {0xB0, 0x01, 0xE0, 0x02, 0x01, 0xE0, 0xE0, 0x0A, 0x20};
	for (uint8_t flag : {uint8_t(0x00), uint8_t(0xB1), uint8_t(0x30), uint8_t(0xE0), uint8_t(0xFF)}) {
		f[0] = flag;
		EXPECT_FALSE(frameAccepted(f, sizeof f)) << int(flag);
	}
}

TEST(IndicatorFrame, MisalignedFrameDropped) {
	// a frame caught from its second byte: the flag lands at the end, byte 0 is a digit
	const uint8_t f[9] = {0x01, 0xE0, 0x02, 0x01, 0xE0, 0xE0, 0x0A, 0x20, 0xB0};
	EXPECT_FALSE(frameAccepted(f, sizeof f));
}

TEST(IndicatorFrame, WrongLengthDropped) {
	const uint8_t f[10] = {0xB0, 0x01, 0xE0, 0x02, 0x01, 0xE0, 0xE0, 0x0A, 0x20, 0x00};
	EXPECT_FALSE(frameAccepted(f, 8));
	EXPECT_FALSE(frameAccepted(f, 10));
	EXPECT_FALSE(frameAccepted(f, 0));
	EXPECT_FALSE(frameAccepted(nullptr, 9));
}

TEST(QuietBus, SendsAtOnceOnABusNeverHeard) {
	QuietBus q;
	EXPECT_TRUE(q.maySend(0, false));
	EXPECT_TRUE(q.maySend(5000, false));
}

TEST(QuietBus, WaitsQuietTimeAfterActivity) {
	QuietBus q;
	q.onActivity(1000);
	EXPECT_FALSE(q.maySend(1000, false));
	EXPECT_FALSE(q.maySend(1000 + QUIET_MS - 1, false));
	EXPECT_TRUE(q.maySend(1000 + QUIET_MS, false));
}

TEST(QuietBus, QuietTimeIsAboutThreeByteTimes) {
	// 1200 Bd, 10 bits per byte -> 8.3 ms per byte; D25 suggests 25 ms
	EXPECT_GE(QUIET_MS, 25u);
	EXPECT_LT(QUIET_MS, 75u);  // shorter than one whole frame
}

TEST(QuietBus, NeverWhileAFrameIsBeingReceived) {
	QuietBus q;
	EXPECT_FALSE(q.maySend(0, true));
	q.onActivity(100);
	EXPECT_FALSE(q.maySend(10000, true));
}

TEST(QuietBus, ReceivingRestartsTheQuietTime) {
	QuietBus q;
	q.onActivity(100);
	EXPECT_FALSE(q.poll(200, true));      // a byte is on the line at 200
	EXPECT_FALSE(q.poll(200 + QUIET_MS - 1, false));
	EXPECT_TRUE(q.poll(200 + QUIET_MS, false));
}

TEST(QuietBus, TickWrapAround) {
	QuietBus q;
	q.onActivity(0xFFFFFFF0u);
	EXPECT_FALSE(q.maySend(0xFFFFFFF0u + 5, false));
	EXPECT_TRUE(q.maySend(0xFFFFFFF0u + QUIET_MS, false));  // wraps past 0
}
