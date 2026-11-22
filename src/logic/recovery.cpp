#include "recovery.hpp"

namespace recovery {
	unsigned cleanupMessageCount() { return 32; }

	void cleanupMessage(unsigned index, uint8_t (&out)[3]) {
		out[0] = static_cast<uint8_t>(0xB0 | ((index >> 1) & 0x0F));
		out[1] = (index & 1) ? 120 : 123;
		out[2] = 0;
	}
}
