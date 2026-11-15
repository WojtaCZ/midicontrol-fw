#pragma once
// States of the base's NowPlaying screen (src/menu.cpp NowPlayingScreen draws them). Time: uint32 ms ticks.

#include <cstdint>

namespace nowplaying {

	enum class State : uint8_t { Playing, Stopping, Stopped };
	enum class Action : uint8_t { SendStop, Leave };

	class Machine {
	public:
		static constexpr uint32_t STOPPED_SHOW_MS	  = 1500;
		static constexpr uint32_t STOPPING_TIMEOUT_MS = 5000; // STOP + MAX_RETRIES x RETRY_TIMEOUT, then SIGNAL_STOPPED

		void   start();						  // SIGNAL_PLAYING
		Action press(uint32_t now);			  // the OK button
		void   stopped(uint32_t now);		  // SIGNAL_STOPPED, or the PC was declared lost
		void   stopFailed();			  // STOP_SONG NAKed or unanswered (PM-1): back to Playing, the next press stops again
		bool   expired(uint32_t now) const;	  // the screen should go now
		State  state() const { return _state; }

	private:
		State	 _state = State::Stopped;
		uint32_t _since = 0;
	};

} // namespace nowplaying
