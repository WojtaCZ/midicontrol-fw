#pragma once
// Rear MIDI A/B LED state (C2, B3): the master "LED" setting gates every rear LED, the MIDI ones included.
// Host tests in test/host/test_led_logic.cpp.
namespace ledlogic {
	enum class MidiLed { Off, Role, Blink };

	// master = Nastaveni > LED; roleId = Ident. MIDI; activity = traffic seen within the blink window.
	constexpr MidiLed midiLed(bool master, bool roleId, bool activity) {
		if (!master) return MidiLed::Off;
		if (activity) return MidiLed::Blink;
		return roleId ? MidiLed::Role : MidiLed::Off;
	}
}
