#include "ram_guard.hpp"

namespace ramguard {

	bool breakAllowed(uintptr_t brk, intptr_t incr, uintptr_t limit, uintptr_t start) {
		if (brk < start || brk > limit) return false;
		if (incr >= 0) return static_cast<uintptr_t>(incr) <= limit - brk;
		return static_cast<uintptr_t>(-incr) <= brk - start;
	}

	size_t untouchedBytes(const uint32_t *lo, const uint32_t *hi) {
		size_t n = 0;
		for (const uint32_t *p = lo; p < hi && *p == PAINT; ++p) n += sizeof(uint32_t);
		return n;
	}

} // namespace ramguard
