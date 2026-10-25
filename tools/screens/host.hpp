#pragma once
// Simulated hardware state behind the stubbed modules; the scenarios in main.cpp set it
#include <cstdint>

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
		uint8_t		requests	= 0; // song list requests made by the GUI
	};
	inline State state;
}
