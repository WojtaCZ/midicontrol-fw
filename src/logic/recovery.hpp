#pragma once
// Stuck-note recovery: the DIN cleanup sent at every boot (C1). Hardware independent;
// host tests in test/host/test_recovery.cpp.
#include <cstdint>

namespace recovery {
	// DIN cleanup (every boot): All Notes Off (CC 123) then All Sound Off (CC 120) for each channel. Index 0..count-1.
	unsigned cleanupMessageCount();
	void	 cleanupMessage(unsigned index, uint8_t (&out)[3]);

	// SYS-PC-03 (D1, D19): fires once on the transition PC present -> PC lost. Never fires while no PC was ever
	// present (manual use keeps the organ powered); re-arms when the PC comes back.
	class PcLossGuard {
	  public:
		bool update(bool present) {
			bool lost = _wasPresent && !present;
			_wasPresent = present;
			return lost;
		}

	  private:
		bool _wasPresent = false;
	};
}
