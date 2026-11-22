// C2: with the master LED off every rear MIDI LED is dark; otherwise blink > role colour > off.
#include <gtest/gtest.h>
#include "led_logic.hpp"

using namespace ledlogic;

TEST(LedLogic, MasterOffIsAlwaysDark) {
	for (bool role : {false, true})
		for (bool act : {false, true}) EXPECT_EQ(midiLed(false, role, act), MidiLed::Off);
}

TEST(LedLogic, MasterOnShowsRoleColourAndBlink) {
	EXPECT_EQ(midiLed(true, false, false), MidiLed::Off);
	EXPECT_EQ(midiLed(true, true, false), MidiLed::Role);
	EXPECT_EQ(midiLed(true, false, true), MidiLed::Blink);
	EXPECT_EQ(midiLed(true, true, true), MidiLed::Blink);
}
