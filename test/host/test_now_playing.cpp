// Base NowPlaying screen states (src/logic/now_playing.*, drawn by src/menu.cpp NowPlayingScreen). SM-1.
#include <gtest/gtest.h>

#include "now_playing.hpp"

using nowplaying::Action;
using nowplaying::Machine;
using nowplaying::State;

// Normal stop: Enter -> STOP sent, "Zastavuji"; SIGNAL_STOPPED -> "Zastaveno" for STOPPED_SHOW_MS, then the screen goes.
TEST(NowPlaying, StopConfirmedByPc) {
	Machine m;
	m.start();
	EXPECT_EQ(m.state(), State::Playing);
	EXPECT_EQ(m.press(1000), Action::SendStop);
	EXPECT_EQ(m.state(), State::Stopping);
	m.stopped(1200);
	EXPECT_EQ(m.state(), State::Stopped);
	EXPECT_FALSE(m.expired(1200 + Machine::STOPPED_SHOW_MS - 1));
	EXPECT_TRUE(m.expired(1200 + Machine::STOPPED_SHOW_MS));
	EXPECT_EQ(m.press(1300), Action::Leave);
}

// SM-1: the PC died, STOP is never answered. "Zastavuji" must not capture the base UI: after
// STOPPING_TIMEOUT_MS the screen goes and the settings menu (manual organ power) is reachable.
TEST(NowPlaying, StoppingTimesOutWithoutPcAnswer) {
	Machine m;
	m.start();
	EXPECT_EQ(m.press(5000), Action::SendStop);
	EXPECT_FALSE(m.expired(5000 + Machine::STOPPING_TIMEOUT_MS - 1));
	EXPECT_TRUE(m.expired(5000 + Machine::STOPPING_TIMEOUT_MS)) << "Stopping never times out: base UI captured";
}

// Pressing again while stopping re-sends STOP and restarts the wait.
TEST(NowPlaying, RepeatedStopRestartsTheWait) {
	Machine m;
	m.start();
	m.press(0);
	EXPECT_EQ(m.press(3000), Action::SendStop);
	EXPECT_FALSE(m.expired(3000 + Machine::STOPPING_TIMEOUT_MS - 1));
	EXPECT_TRUE(m.expired(3000 + Machine::STOPPING_TIMEOUT_MS));
}

// While playing the screen never expires on its own.
TEST(NowPlaying, PlayingNeverExpires) {
	Machine m;
	m.start();
	EXPECT_FALSE(m.expired(0));
	EXPECT_FALSE(m.expired(0x7FFFFFFFu));
}

// Timeouts across the uint32 ms tick wrap.
TEST(NowPlaying, TimeoutsAcrossTickWrap) {
	Machine	 m;
	uint32_t t = 0xFFFFFFFFu - 100u;
	m.start();
	m.press(t);
	EXPECT_FALSE(m.expired(t + 200u));
	EXPECT_TRUE(m.expired(t + Machine::STOPPING_TIMEOUT_MS));
	m.stopped(t + 50u);
	EXPECT_FALSE(m.expired(t + 60u));
	EXPECT_TRUE(m.expired(t + 50u + Machine::STOPPED_SHOW_MS));
}

// A new SIGNAL_PLAYING while stopping (the PC started the next song) is Playing again.
TEST(NowPlaying, StartWhileStoppingIsPlaying) {
	Machine m;
	m.start();
	m.press(0);
	m.start();
	EXPECT_EQ(m.state(), State::Playing);
	EXPECT_FALSE(m.expired(Machine::STOPPING_TIMEOUT_MS * 10));
}

// PM-1/UI2-1: the STOP was NAKed or never answered (PC alive): the song still plays, so the screen
// goes back to Playing and one more press sends STOP again.
TEST(NowPlaying, StopFailedReturnsToPlaying) {
	Machine m;
	m.start();
	m.press(100);
	m.stopFailed();
	EXPECT_EQ(m.state(), State::Playing) << "a failed STOP must not leave the screen in Zastavuji";
	EXPECT_FALSE(m.expired(100 + Machine::STOPPING_TIMEOUT_MS));
	EXPECT_EQ(m.press(200), Action::SendStop);
}

// A late failure after SIGNAL_STOPPED changes nothing.
TEST(NowPlaying, StopFailedAfterStoppedIsIgnored) {
	Machine m;
	m.start();
	m.press(0);
	m.stopped(10);
	m.stopFailed();
	EXPECT_EQ(m.state(), State::Stopped);
}
