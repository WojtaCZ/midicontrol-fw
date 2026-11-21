#include <gtest/gtest.h>
#include "recording.hpp"

using namespace recording;
using routercore::Failure;
using protocol::NakReason;

TEST(Recording, PressStartsWhenIdle) {
	Machine m;
	EXPECT_EQ(m.press(), Action::SendStart);
	EXPECT_EQ(m.state(), State::Starting);
}

TEST(Recording, SecondPressWhileStartingDoesNothing) {
	Machine m;
	m.press();
	EXPECT_EQ(m.press(), Action::None);
	EXPECT_EQ(m.state(), State::Starting);
}

TEST(Recording, SignalRecordingThenPressStops) {
	Machine m;
	m.press();
	m.signalRecording();
	EXPECT_EQ(m.state(), State::Recording);
	EXPECT_EQ(m.press(), Action::SendStop);
	EXPECT_EQ(m.state(), State::Stopping);
	m.signalStopped();
	EXPECT_EQ(m.state(), State::Idle);
}

TEST(Recording, SignalFromAnotherSourceIsEnough) {
	Machine m; // the PC started it (console / web): the base follows
	m.signalRecording();
	EXPECT_EQ(m.state(), State::Recording);
	EXPECT_TRUE(m.recordingOrStarting());
}

TEST(Recording, NakBusyMapsToBusy) {
	Machine m;
	m.press();
	m.requestFailed(Failure::Nak, NakReason::BUSY);
	EXPECT_EQ(m.state(), State::Busy);
	EXPECT_FALSE(m.recordingOrStarting());
}

TEST(Recording, NakUnknownMsgMapsToNoSupport) {
	Machine m;
	m.press();
	m.requestFailed(Failure::Nak, NakReason::UNKNOWN_MSG);
	EXPECT_EQ(m.state(), State::NoSupport);
}

TEST(Recording, OtherFailuresMapToFailed) {
	for (auto f : {Failure::NoAnswer, Failure::NotSent}) {
		Machine m;
		m.press();
		m.requestFailed(f, NakReason::UNKNOWN_MSG);
		EXPECT_EQ(m.state(), State::Failed);
	}
	Machine m;
	m.press();
	m.requestFailed(Failure::Nak, NakReason::INVALID_PAYLOAD);
	EXPECT_EQ(m.state(), State::Failed);
}

TEST(Recording, PressAfterARefusalTriesAgain) {
	Machine m;
	m.press();
	m.requestFailed(Failure::Nak, NakReason::BUSY);
	EXPECT_EQ(m.press(), Action::SendStart);
	EXPECT_EQ(m.state(), State::Starting);
}

TEST(Recording, StartAttemptEndedByStoppedGoesIdle) {
	Machine m; // the PC ACKed but the recorder never ran: SIGNAL_STOPPED to the base
	m.press();
	m.signalStopped();
	EXPECT_EQ(m.state(), State::Idle);
}

TEST(Recording, FailedStopKeepsRecording) {
	Machine m;
	m.press();
	m.signalRecording();
	m.press();
	m.requestFailed(Failure::NoAnswer, NakReason::UNKNOWN_MSG);
	EXPECT_EQ(m.state(), State::Recording);
}

TEST(Recording, PcLostGoesIdle) {
	Machine m;
	m.signalRecording();
	m.pcLost();
	EXPECT_EQ(m.state(), State::Idle);
}

TEST(Recording, LateFailureOutsideAnAttemptIsIgnored) {
	Machine m;
	m.requestFailed(Failure::Nak, NakReason::BUSY);
	EXPECT_EQ(m.state(), State::Idle);
}
