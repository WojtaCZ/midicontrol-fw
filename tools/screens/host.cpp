// Stubbed hardware modules of the base for the screen export tool
#include "host.hpp"
#include "oled.hpp"
#include "base.hpp"
#include "bluetooth.hpp"
#include "display.hpp"
#include "router.hpp"
#include "menu.hpp"
#include "settings.hpp"
#include <stmcpp/systick.hpp>

#include <cstdio>

Oled::OledBuffer frameBuffer;

namespace Oled {
	void update() {}
	bool isSleeping() { return host::state.sleeping; }
	void wakeupCallback() {
		host::state.sleeping = false;
		host::state.lastWake = stmcpp::systick::ticks_;
	}
	void flush(uint32_t) {}
}

namespace base::current {
	bool isEnabled() { return host::state.power; }
	void enable() { host::state.power = true; }
	void disable() { host::state.power = false; }
}

namespace Bluetooth {
	bool isUp() { return host::state.bleUp; }
	uint8_t startPairing() { return 0; }
	bool isConnected() { return host::state.connected; }
	bool isPeerAccepted() { return host::state.connected; }
	const char *name() { return "MIDIControlBase"; }
	stmcpp::units::duration getDataReceivedTimestamp() { return stmcpp::units::duration::fromSeconds(0); }
	RestartState restartState() { return RestartState::IDLE; }
	ConfigState configState() { return ConfigState::DONE; }
	bool resetConfirmed() { return true; }
	uint32_t rxByteCount() { return 1234; }
}

namespace display {
	void formatSong(char (&out)[5]) { std::snprintf(out, sizeof(out), "%4u", static_cast<unsigned>(host::state.song % 10000)); }
	void formatVerse(char (&out)[3]) { std::snprintf(out, sizeof(out), "%2u", static_cast<unsigned>(host::state.verse % 100)); }
	char getLetter() { return host::state.letter; }
	ledColor getLed() { return GREEN; }
	bool isConnected() { return host::state.display; }
}

namespace router {
	bool isPcAlive() { return host::state.pcAlive; }
	bool isRemoteAlive() { return host::state.connected; }
	void emergencyAbort(bool) {
		const bool organWasOn = host::state.power;
		host::state.power = false;
		host::state.aborts++;
		GUI::abortAll(organWasOn);
	}
}

namespace settings {
	void save() {}
	bool ledMasterEnable() { return host::state.ledMaster; }
	void setLedMasterEnable(bool v) { host::state.ledMaster = v; }
	bool midiRoleIdentifier() { return host::state.midiRole; }
	void setMidiRoleIdentifier(bool v) { host::state.midiRole = v; }
	const char *remoteAddr() { return host::state.remoteAddr; }
}
