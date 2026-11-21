#pragma once
// A7: the base's side of "Nahraj" (RECORD_SONG 0x12 Base -> PC). Pure logic, no hardware: the menu and the router
// feed events, the machine says what to send and what the state is. The screens that show the state ("Priprava",
// "Nahravam", "Zaneprazdneno", "PC nema nahravani") are task C3.

#include "protocol/types.hpp"
#include "router_core.hpp"
#include <cstdint>

namespace recording {

	enum class State : uint8_t {
		Idle,		// nothing recording
		Starting,	// RECORD_SONG [1] sent, waiting for SIGNAL_RECORDING (the organ warm-up takes seconds)
		Recording,	// SIGNAL_RECORDING seen
		Stopping,	// RECORD_SONG [0] sent, waiting for SIGNAL_STOPPED
		Busy,		// the PC answered NAK BUSY (a recording or song runs, or an update): C3 "Zaneprazdneno"
		NoSupport,	// the PC answered NAK UNKNOWN_MSG (protocol v1 PC): C3 "PC nema nahravani"
		Failed		// any other NAK, no answer, not sent
	};
	enum class Action : uint8_t { None, SendStart, SendStop };

	// What a failed RECORD_SONG means for the state: NAK BUSY, NAK UNKNOWN_MSG, anything else.
	State failureState(routercore::Failure f, protocol::NakReason reason);

	class Machine {
	public:
		Action press();					// the "Nahraj" item: starts when idle (or after a refusal), stops when recording
		void   signalRecording();		// SIGNAL_RECORDING (from the PC, or the copy forwarded to the remote)
		void   signalStopped();			// SIGNAL_STOPPED: the recording (or the start attempt) ended
		void   requestFailed(routercore::Failure f, protocol::NakReason reason);	// RECORD_SONG NAKed / unanswered
		void   pcLost();				// the PC was declared lost
		State  state() const { return _state; }
		bool   recordingOrStarting() const { return _state == State::Starting || _state == State::Recording || _state == State::Stopping; }

	private:
		State _state = State::Idle;
	};

} // namespace recording
