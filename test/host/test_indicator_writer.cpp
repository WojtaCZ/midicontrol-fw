// Host tests for indicator::WriterTracker: who wrote the number on the shared multi-master bus last (A3, Q2-19,
// SYS-BUS-07). NOW_PLAYING_INFO [NPI_CLEAR_IF_OWN] clears only while the base's own number is still on the bus.
#include <gtest/gtest.h>
#include "indicator_bus.hpp"

using indicator::WriterTracker;

namespace {
	const uint8_t kFrame[9] = {0xB0, 0x01, 0xE0, 0x02, 0x01, 0xE0, 0xE0, 0x0A, 0x20};
}

TEST(WriterTracker, NothingWrittenNothingToClear) {
	WriterTracker t;
	EXPECT_FALSE(t.mayClear());
}

TEST(WriterTracker, OwnWriteAllowsClear) {
	WriterTracker t;
	t.onOwnWrite();
	EXPECT_TRUE(t.mayClear());
}

TEST(WriterTracker, ForeignFrameAfterOwnWriteBlocksClear) {
	WriterTracker t;
	t.onOwnWrite();
	t.onRxFrame(kFrame, false);
	EXPECT_FALSE(t.mayClear());
}

TEST(WriterTracker, OwnWriteAfterForeignAllowsClearAgain) {
	WriterTracker t;
	t.onOwnWrite();
	t.onRxFrame(kFrame, false);
	t.onOwnWrite();
	EXPECT_TRUE(t.mayClear());
}

TEST(WriterTracker, EchoOfOwnTxIsNotForeign) {
	WriterTracker t;
	t.onOwnWrite();
	t.onRxFrame(kFrame, true);
	EXPECT_TRUE(t.mayClear());
}

TEST(WriterTracker, ClearedBusHasNoOwnerLeft) {
	WriterTracker t;
	t.onOwnWrite();
	ASSERT_TRUE(t.mayClear());
	t.onCleared();
	EXPECT_FALSE(t.mayClear());
	EXPECT_EQ(t.writer(), WriterTracker::Writer::None);
}

TEST(WriterTracker, SkippedClearLeavesForeign) {
	WriterTracker t;
	t.onOwnWrite();
	t.onRxFrame(kFrame, false);
	EXPECT_FALSE(t.tryClear());
	EXPECT_EQ(t.writer(), WriterTracker::Writer::Foreign);
	EXPECT_FALSE(t.tryClear()); // still skipped, stays foreign
}

TEST(WriterTracker, TryClearOnOwnClearsAndForgets) {
	WriterTracker t;
	t.onOwnWrite();
	EXPECT_TRUE(t.tryClear());
	EXPECT_EQ(t.writer(), WriterTracker::Writer::None);
	EXPECT_FALSE(t.tryClear()); // nothing of ours left
}

TEST(WriterTracker, UnconditionalClearAlwaysWorksAndResetsWriter) {
	for (int start = 0; start < 3; ++start) {
		WriterTracker t;
		if (start == 1) t.onOwnWrite();
		if (start == 2) t.onRxFrame(kFrame, false);
		t.onCleared(); // the unconditional clear path (empty NOW_PLAYING_INFO)
		EXPECT_EQ(t.writer(), WriterTracker::Writer::None) << start;
	}
}

TEST(WriterTracker, ForeignFromNoneStaysForeign) {
	WriterTracker t;
	t.onRxFrame(kFrame, false);
	EXPECT_EQ(t.writer(), WriterTracker::Writer::Foreign);
	EXPECT_FALSE(t.mayClear());
}

TEST(WriterTracker, IsTiny) {
	EXPECT_EQ(sizeof(WriterTracker), 1u);
}
