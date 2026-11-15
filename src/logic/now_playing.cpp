#include "now_playing.hpp"

namespace nowplaying {

	void Machine::start() {
		_state = State::Playing;
	}

	Action Machine::press(uint32_t now) {
		if (_state == State::Stopped) return Action::Leave;
		// Stop (again while stopping: the PC may have missed it)
		_state = State::Stopping;
		_since = now;
		return Action::SendStop;
	}

	void Machine::stopped(uint32_t now) {
		_state = State::Stopped;
		_since = now;
	}

	void Machine::stopFailed() {
		if (_state == State::Stopping) _state = State::Playing;
	}

	bool Machine::expired(uint32_t now) const {
		if (_state == State::Stopped) return now - _since >= STOPPED_SHOW_MS;
		// SM-1: no SIGNAL_STOPPED (PC gone or STOP lost): do not hold the base UI, the settings menu stays reachable
		if (_state == State::Stopping) return now - _since >= STOPPING_TIMEOUT_MS;
		return false;
	}

} // namespace nowplaying
