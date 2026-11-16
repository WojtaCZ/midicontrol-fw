#include <gtest/gtest.h>
#include "organ_source.hpp"

using organsrc::Source;
using organsrc::Tracker;

TEST(OrganSource, OffReportsZeroWhateverTheLastSource) {
	Tracker t;
	t.setSource(Source::PC);
	EXPECT_EQ(t.code(false), 0);
}

TEST(OrganSource, PcCommandReportsPc) {
	Tracker t;
	t.setSource(Source::PC);
	EXPECT_EQ(t.code(true), 1);
}

TEST(OrganSource, MenuReportsMenu) {
	Tracker t;
	t.setSource(Source::MENU);
	EXPECT_EQ(t.code(true), 2);
}

TEST(OrganSource, OnWithoutAnyRecordedSourceIsUnknown) {
	Tracker t;
	EXPECT_EQ(t.code(true), 3);
}

TEST(OrganSource, DisableForgetsTheSource) {
	Tracker t;
	t.setSource(Source::PC);
	t.clear();
	EXPECT_EQ(t.code(true), 3); // on again without a new source: unknown, never a stale PC
	EXPECT_EQ(t.code(false), 0);
}

TEST(OrganSource, LastEnablerWins) {
	Tracker t;
	t.setSource(Source::MENU);
	t.setSource(Source::PC);
	EXPECT_EQ(t.code(true), 1);
}

TEST(OrganSource, SetCurrentSourceFromAddress) {
	EXPECT_EQ(organsrc::fromSender(protocol::ADDR_PC), Source::PC);
	EXPECT_EQ(organsrc::fromSender(protocol::ADDR_REMOTE), Source::UNKNOWN);
}
