#pragma once
// States of the base's NowPlaying screen (src/menu.cpp NowPlayingScreen draws them). Time: uint32 ms ticks.

#include <cstdint>

namespace nowplaying {

	enum class State : uint8_t { Playing, Stopping, Stopped };
	enum class Action : uint8_t { SendStop, SendAddVerse, Leave };
	enum class Selection : uint8_t { Stop, AddVerse };

	class Machine {
	public:
		static constexpr uint32_t STOPPED_SHOW_MS	  = 1500;
		static constexpr uint32_t STOPPING_TIMEOUT_MS = 5000; // STOP + MAX_RETRIES x RETRY_TIMEOUT, then SIGNAL_STOPPED

		void   start();						  // SIGNAL_PLAYING (a new song: selection back to Stop, no anchor yet)
		void   forgetAnchor() { _anchor = false; _sel = Selection::Stop; } // another song started while the screen was up
		void   anchorSeen();			  // SIGNAL_ANCHOR: "Pridat sloku" becomes selectable (C4)
		void   toggle();				  // encoder Up/Down: Stop <-> Pridat sloku (only while Playing and after an anchor)
		Selection selection() const { return _sel; }
		bool   addVerseAvailable() const { return _anchor && _state == State::Playing; }
		void   _unused();						  // SIGNAL_PLAYING
		Action press(uint32_t now);			  // the OK button
		void   stopped(uint32_t now);		  // SIGNAL_STOPPED, or the PC was declared lost
		void   stopFailed();			  // STOP_SONG NAKed or unanswered (PM-1): back to Playing, the next press stops again
		bool   expired(uint32_t now) const;	  // the screen should go now
		State  state() const { return _state; }

	private:
		State	 _state = State::Stopped;
		Selection _sel = Selection::Stop;
		bool	 _anchor = false;
		uint32_t _since = 0;
	};

} // namespace nowplaying
