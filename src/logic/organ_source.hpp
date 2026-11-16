#pragma once
// Who switched the organ power (SSR) on, reported as INFO tag ORGAN_SRC (A5, Q2-33): 0 off, 1 on by the PC
// (SET_CURRENT from ADDR_PC), 2 on by the base menu, 3 on, source unknown. After a PC crash the restarted PC reads
// it to tell its own power-on from a manual one (BE-4). The SSR is always off after a base reset (PA4 low from
// the static GPIO init), so the tracker starts empty and 3 only appears if a switch-on path forgets to record a source.
// "Last enabler wins": an explicit SET_CURRENT 1 from the PC while the menu had it on makes the PC the owner.

#include <cstdint>

#include "protocol/constants.hpp"

namespace organsrc {

	enum class Source : uint8_t { NONE = 0, PC = 1, MENU = 2, UNKNOWN = 3 };

	// Source recorded for a SET_CURRENT received from `sender` (ADDR_*): only the PC counts as the PC
	constexpr Source fromSender(uint8_t sender) { return sender == protocol::ADDR_PC ? Source::PC : Source::UNKNOWN; }

	class Tracker {
	public:
		void setSource(Source s) { _src = s; }
		void clear() { _src = Source::NONE; } // power switched off
		// ORGAN_SRC value for the actual SSR state
		uint8_t code(bool on) const {
			if (!on) return 0;
			return _src == Source::NONE ? static_cast<uint8_t>(Source::UNKNOWN) : static_cast<uint8_t>(_src);
		}

	private:
		Source _src = Source::NONE;
	};

} // namespace organsrc
