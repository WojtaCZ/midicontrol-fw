#pragma once
// Simulated hardware state behind the stubbed modules; the scenarios in main.cpp set it
#include <cstdint>
#include <string>
#include <vector>

namespace host {
	struct State {
		bool		bleUp		= true;
		bool		connected	= true;
		bool		pcAlive		= true;
		bool		power		= false; // organ power SSR
		bool		ledMaster	= true;
		bool		midiRole	= true;
		const char *remoteAddr	= "D88039F1A2B4"; // "" = no remote paired yet
		bool		display		= true; // church song display connected
		uint16_t	song		= 406;
		uint8_t		verse		= 2;
		char		letter		= 'A';
		uint8_t		aborts		= 0; // router::emergencyAbort calls (each forwards 0x15 to the PC when it is alive)
		uint8_t		requests	= 0; // song list requests made by the GUI
		bool		sleeping	= false; // OLED off: the first key only wakes (driver sets it after 30 s idle)
		uint32_t	lastWake	= 0;	 // systick of the last wakeupCallback
	};
	inline State state;

	// Text drawn by the GUI since the last clear (recorded by stubs/gfx/text.hpp)
	struct Text {
		int			x, y, h;
		std::string s;
	};
	inline std::vector<Text> texts;
}
