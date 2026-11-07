#pragma once
// Stuck-note recovery after a watchdog/fault reset (TODO "Stuck base / stuck notes"). Hardware independent;
// host tests in test/host/test_recovery.cpp.
#include <cstdint>

namespace recovery {
	// csr = RCC->CSR as read at boot (before it is cleared); unannouncedFault = a fault record that no banner has shown yet.
	// True after an IWDG/WWDG reset, or a software reset that follows a fault (loop stall, HardFault, error).
	bool needsNoteCleanup(uint32_t csr, bool unannouncedFault);

	// DIN cleanup: All Notes Off (CC 123) then All Sound Off (CC 120) for each channel. Index 0..count-1.
	unsigned cleanupMessageCount();
	void	 cleanupMessage(unsigned index, uint8_t (&out)[3]);
}
