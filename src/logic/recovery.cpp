#include "recovery.hpp"

namespace recovery {
	static constexpr uint32_t CSR_SFTRSTF  = 1u << 28; // RM0440 RCC_CSR
	static constexpr uint32_t CSR_IWDGRSTF = 1u << 29;
	static constexpr uint32_t CSR_WWDGRSTF = 1u << 30;

	bool needsNoteCleanup(uint32_t csr, bool unannouncedFault) {
		if (csr & (CSR_IWDGRSTF | CSR_WWDGRSTF)) return true;
		return (csr & CSR_SFTRSTF) && unannouncedFault;
	}

	unsigned cleanupMessageCount() { return 32; }

	void cleanupMessage(unsigned index, uint8_t (&out)[3]) {
		out[0] = static_cast<uint8_t>(0xB0 | ((index >> 1) & 0x0F));
		out[1] = (index & 1) ? 120 : 123;
		out[2] = 0;
	}
}
