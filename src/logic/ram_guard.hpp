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

	// Stack overflow (silent: no MPU): the stack pointer fell below the heap break, or the paint word right above the break was
	// overwritten (the stack reached the heap; the paint is never touched by the heap itself).
	inline bool stackBreached(uintptr_t sp, uintptr_t heapBreak, uint32_t canaryWord) { return sp < heapBreak || canaryWord != PAINT; }

	// Can a fault handler read a stacked exception frame (8 words) at `addr` without faulting itself? Word aligned and
	// entirely inside [ramLo, ramHi).
	inline bool frameInRam(uintptr_t addr, uintptr_t ramLo, uintptr_t ramHi) { return addr != 0 && (addr & 3u) == 0 && addr >= ramLo && addr + 32u <= ramHi; }

} // namespace ramguard
