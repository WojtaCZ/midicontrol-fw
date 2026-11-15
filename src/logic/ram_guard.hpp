#pragma once
// RAM guards (audit TL-1): the heap break limit for _sbrk and the stack high-water scan.
// Pure functions, host-tested; the firmware glue is in src/ram.cpp.

#include <cstddef>
#include <cstdint>

namespace ramguard {

	// Fill pattern of the free RAM between the heap break and the stack at boot
	inline constexpr uint32_t PAINT = 0xC5C5C5C5u;

	// May the heap break move from `brk` by `incr` bytes? It must stay within [start, limit].
	bool breakAllowed(uintptr_t brk, intptr_t incr, uintptr_t limit, uintptr_t start);

	// Bytes of intact paint from `lo` upwards until the first overwritten word (the deepest stack use reached
	// from above, or heap data from below). 0 for an empty range.
	size_t untouchedBytes(const uint32_t *lo, const uint32_t *hi);

} // namespace ramguard
