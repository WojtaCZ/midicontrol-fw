#include <gtest/gtest.h>
#include <algorithm>
#include <vector>
#include "song_window.hpp"

using songwin::Window;

namespace {
	// Simulated PC list of `total` entries; applies a Plan to a model of the menu's name array (absolute indexes).
	struct Rig {
		int total;
		int perPage;
		Window w;
		std::vector<int> names;	 // absolute index per item
		uint16_t lastOffset = 0xFFFF;
		int requests = 0;

		Rig(int t, int pp) : total(t), perPage(pp) {}

		void respond(uint16_t offset) {
			int n = 0;
			if (offset < total) n = std::min(perPage, total - offset);
			bool more = offset + n < total;
			Window::Plan p = w.onResponse(n, more);
			ASSERT_TRUE(p.ok);
			names.erase(names.begin(), names.begin() + p.dropFront);
			for (int i = 0; i < p.keepNew; ++i) names.push_back(offset + i);
			ASSERT_EQ(static_cast<int>(names.size()), p.count);
			ASSERT_EQ(p.count, w.count());
			sel = p.cursor;
		}
		void open() {
			uint16_t off = w.startFirst();
			lastOffset = off;
			++requests;
			respond(off);
		}
		bool down() {
			if (sel < w.count()) ++sel;  // the "Zpet" item is index count
			uint16_t off;
			if (!w.downRequest(sel, off)) return false;
			lastOffset = off;
			++requests;
			respond(off);
			return true;
		}
		bool up() {
			int before = sel;
			if (sel > 0) --sel;
			uint16_t off;
			if (!w.upRequest(before, off)) return false;
			lastOffset = off;
			++requests;
			respond(off);
			return true;
		}
		int abs() const { return names[static_cast<size_t>(sel)]; }
		int sel = 0;
	};
}

TEST(SongWindow, FirstPageStartsAtZero) {
	Rig r(300, 20);
	r.open();
	EXPECT_EQ(r.w.windowStart(), 0);
	EXPECT_EQ(r.w.count(), 20);
	EXPECT_TRUE(r.w.hasMore());
	EXPECT_EQ(r.sel, 0);
}

TEST(SongWindow, DownNearEndAsksForStartPlusCountAndSlidesWindow) {
	Rig r(300, 20);
	r.open();
	for (int i = 0; i < 17; ++i) EXPECT_FALSE(r.down());	// cursor 17 of 20: not yet within PREFETCH of the end
	EXPECT_TRUE(r.down());									// cursor 18 = count - 2
	EXPECT_EQ(r.lastOffset, 20);
	EXPECT_EQ(r.w.count(), 24);								// 20 + 20 capped at 24
	EXPECT_EQ(r.w.windowStart(), 16);
	EXPECT_EQ(r.abs(), 18);									// cursor stays on the same absolute entry
	EXPECT_EQ(r.names.front(), 16);
	EXPECT_EQ(r.names.back(), 39);
}

TEST(SongWindow, ScrollToTheEndOf300WithoutLosingOrRepeating) {
	Rig r(300, 20);
	r.open();
	int guard = 0;
	int lastAbs = 0;
	while (guard++ < 2000) {
		r.down();
		if (r.sel >= r.w.count()) break;	// reached "Zpet"
		EXPECT_EQ(r.abs(), lastAbs + 1);	// each Down moves exactly one entry: none lost, none repeated
		lastAbs = r.abs();
	}
	EXPECT_EQ(lastAbs, 299);
	EXPECT_EQ(r.names.back(), 299);
	EXPECT_FALSE(r.w.hasMore());
	EXPECT_LE(r.w.count(), Window::CAP);
}

TEST(SongWindow, OffsetsBeyond255AreRequestedAbove255) {
	Rig r(600, 20);
	r.open();
	uint16_t maxOff = 0;
	for (int i = 0; i < 700; ++i) {
		r.down();
		if (r.lastOffset != 0xFFFF && r.lastOffset > maxOff) maxOff = r.lastOffset;
	}
	EXPECT_GT(maxOff, 255);
	EXPECT_EQ(r.names.back(), 599);
}

TEST(SongWindow, UpAtFirstItemRebuildsFromStartMinus12) {
	Rig r(300, 20);
	r.open();
	while (r.w.windowStart() < 100) r.down();
	int startBefore = r.w.windowStart();
	// go up to the first item of the window
	while (r.sel > 0) EXPECT_FALSE(r.up());
	int firstAbs = r.abs();
	EXPECT_EQ(firstAbs, startBefore);
	EXPECT_TRUE(r.up());
	EXPECT_EQ(r.lastOffset, startBefore - Window::UP_STEP);
	EXPECT_EQ(r.w.windowStart(), startBefore - 12);
	EXPECT_EQ(r.abs(), firstAbs - 1);		// lands on the previous entry by absolute index
	EXPECT_EQ(r.w.count(), 20);
}

TEST(SongWindow, UpNearZeroClampsOffsetAndNoUpRequestAtTheTop) {
	Rig r(300, 20);
	r.open();
	while (r.w.windowStart() < 5) r.down();
	int guard = 0;
	while (r.w.windowStart() > 0 && guard++ < 10) {
		while (r.sel > 0) r.up();
		if (r.w.windowStart() > 0) EXPECT_TRUE(r.up());
		EXPECT_LE(r.lastOffset, r.w.windowStart() + 0);
	}
	EXPECT_EQ(r.w.windowStart(), 0);
	EXPECT_EQ(r.lastOffset, 0);				// max(0, start - 12) clamps at 0
	while (r.sel > 0) EXPECT_FALSE(r.up());
	uint16_t off;
	EXPECT_FALSE(r.w.upRequest(0, off));	// start == 0: nothing above
}

TEST(SongWindow, ShortPagesAfterUpStillWork) {
	Rig r(300, 5);	// pages of 5 entries: Up by 12 returns fewer than 12
	r.open();
	for (int i = 0; i < 80; ++i) r.down();
	int startBefore = r.w.windowStart();
	while (r.sel > 0) r.up();
	EXPECT_TRUE(r.up());
	EXPECT_EQ(r.w.windowStart(), startBefore - 12);
	EXPECT_EQ(r.w.count(), 5);
	EXPECT_EQ(r.sel, 4);					// clamped to the last entry that arrived
	// back down: the next request starts right after the window
	for (int i = 0; i < 400; ++i) r.down();
	EXPECT_EQ(r.names.back(), 299);
}

TEST(SongWindow, PageLargerThanWindowIsTruncatedAndOffsetFollowsKeptEntries) {
	Window w;
	w.startFirst();
	Window::Plan p = w.onResponse(40, false);	// 40 codes in one response (short codes), PC says no more
	EXPECT_EQ(p.count, Window::CAP);
	EXPECT_TRUE(w.hasMore());					// forced: 16 codes were not kept
	uint16_t off;
	ASSERT_TRUE(w.downRequest(23, off));
	EXPECT_EQ(off, 24);							// right after the last kept entry, not 40
}

TEST(SongWindow, DownPageCannotDropTheWholeOldWindowUnlessFull) {
	Window w;
	w.startFirst();
	w.onResponse(24, true);
	uint16_t off;
	ASSERT_TRUE(w.downRequest(23, off));
	Window::Plan p = w.onResponse(24, true);
	EXPECT_EQ(p.dropFront, 22);				// bench 2.12: the cursor entry (23) and the one above it stay
	EXPECT_EQ(p.keepNew, 22);				// so only 22 of the 24 new codes fit
	EXPECT_EQ(w.windowStart(), 22);
	EXPECT_EQ(p.cursor, 1);					// still on absolute entry 23
	EXPECT_TRUE(w.hasMore());
	ASSERT_TRUE(w.downRequest(22, off));
	EXPECT_EQ(off, 46);						// right after the last kept code: nothing skipped
}

// Bench 2.12 (2026-11-22): the PC sends 25-27 short codes per page, more than CAP; each page load pushed the cursor
// 2 entries forward (60 Downs ended on index 64).
TEST(SongWindow, ScrollWithPagesLargerThanTheWindowMovesOneEntryPerDown) {
	for (int pp : {24, 25, 27, 40}) {
		Rig r(411, pp);
		r.open();
		int lastAbs = 0, guard = 0;
		while (guard++ < 3000) {
			r.down();
			if (r.sel >= r.w.count()) break;
			ASSERT_EQ(r.abs(), lastAbs + 1) << "perPage " << pp;
			lastAbs = r.abs();
		}
		EXPECT_EQ(lastAbs, 410) << "perPage " << pp;
		EXPECT_LE(r.w.count(), Window::CAP);
	}
}

TEST(SongWindow, DownPressesWhileLoadingAreKept) {
	Window w;
	w.startFirst();
	w.onResponse(24, true);
	uint16_t off;
	ASSERT_TRUE(w.downRequest(22, off));	// request at entry 22
	// two more Downs while waiting: the menu is on the "Zpet" item (24)
	Window::Plan p = w.onResponse(25, true, 24);
	const int abs = w.windowStart() + p.cursor;
	EXPECT_EQ(abs, 24);						// first new entry, not back on 22
	EXPECT_LE(p.count, Window::CAP);
}

TEST(SongWindow, DownPressesWhileUpLoadsAreKept) {
	Window w;
	w.startFirst();
	w.onResponse(24, true);
	uint16_t off;
	ASSERT_TRUE(w.downRequest(22, off));
	w.onResponse(24, true);					// window now starts at 21 (cursor entry 22 and the one above stay)
	ASSERT_EQ(w.windowStart(), 21);
	ASSERT_TRUE(w.upRequest(0, off));		// Up at the first item: wants entry 20
	Window::Plan p = w.onResponse(24, true, 2);	// two Downs while waiting: entry 20 + 2 = 22
	EXPECT_EQ(w.windowStart() + p.cursor, 22);
}

TEST(SongWindow, EmptyResponseEndsDownPagingAndKeepsWindow) {
	Window w;
	w.startFirst();
	w.onResponse(10, true);
	uint16_t off;
	ASSERT_TRUE(w.downRequest(9, off));
	Window::Plan p = w.onResponse(0, false);
	EXPECT_TRUE(p.ok);
	EXPECT_EQ(p.count, 10);
	EXPECT_EQ(p.dropFront, 0);
	EXPECT_EQ(p.keepNew, 0);
	EXPECT_FALSE(w.hasMore());
	EXPECT_FALSE(w.downRequest(9, off));
}

TEST(SongWindow, NoSecondRequestWhileWaitingAndTimeoutAllowsRetry) {
	Window w;
	w.startFirst();
	w.onResponse(10, true);
	uint16_t off;
	ASSERT_TRUE(w.downRequest(9, off));
	EXPECT_EQ(w.state(), Window::State::WaitingMore);
	EXPECT_FALSE(w.downRequest(9, off));
	uint16_t up;
	EXPECT_FALSE(w.upRequest(0, up));
	w.abort();	// timeout (3 s) or NAK
	EXPECT_EQ(w.state(), Window::State::Idle);
	ASSERT_TRUE(w.downRequest(9, off));
	EXPECT_EQ(off, 10);
}

TEST(SongWindow, ResponseWhileIdleIsIgnored) {
	Window w;
	Window::Plan p = w.onResponse(5, false);
	EXPECT_FALSE(p.ok);
	w.startFirst();
	w.onResponse(5, false);
	EXPECT_FALSE(w.onResponse(5, false).ok);
}

TEST(SongWindow, NoRequestWithoutMoreEntries) {
	Window w;
	w.startFirst();
	w.onResponse(10, false);
	uint16_t off;
	EXPECT_FALSE(w.downRequest(9, off));
	EXPECT_FALSE(w.downRequest(10, off));
}
