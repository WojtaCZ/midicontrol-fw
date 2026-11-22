#include "recording.hpp"

namespace recording {

	State failureState(routercore::Failure f, protocol::NakReason reason) {
		if (f == routercore::Failure::Nak) {
			if (reason == protocol::NakReason::BUSY) return State::Busy;
			if (reason == protocol::NakReason::UNKNOWN_MSG) return State::NoSupport;
		}
		return State::Failed;
	}

	Action Machine::press() {
		switch (_state) {
			case State::Recording:
				_state = State::Stopping;
				return Action::SendStop;
			case State::Stopping:
				return Action::SendStop; // the PC may have missed it: same as the Now Playing screen
			case State::Starting:
				return Action::None;
			default:
				_state = State::Starting;
				return Action::SendStart;
		}
	}

	void Machine::signalRecording() {
		_state = State::Recording;
	}

	void Machine::signalStopped() {
		_state = State::Idle;
	}

	void Machine::requestFailed(routercore::Failure f, protocol::NakReason reason) {
		if (_state == State::Starting) _state = failureState(f, reason);
		else if (_state == State::Stopping) _state = State::Recording; // the stop did not arrive: still recording
	}

	void Machine::dismiss() {
		if (_state == State::Busy || _state == State::NoSupport || _state == State::Failed) _state = State::Idle;
	}

	void Machine::pcLost() {
		_state = State::Idle;
	}

} // namespace recording
