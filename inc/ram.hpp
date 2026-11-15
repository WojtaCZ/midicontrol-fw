#pragma once
// RAM guards (audit TL-1): a limited heap (_sbrk) and a painted stack with a high-water mark.
// Readout: "stack" command on the log CDC (debug only, no protocol change).

#include <cstddef>
#include <cstdint>

namespace ram {

	// Fill the free RAM between the heap break and the current stack pointer with the paint pattern.
	// Call first thing in main() (shallow stack).
	void paintStack();

	// Smallest free gap seen so far between the heap break and the deepest stack use, in bytes
	size_t stackUntouched();

	// Heap in use (break - start of the heap) and the limit it may reach, in bytes
	size_t heapUsed();
	size_t heapLimit();

	// How many _sbrk requests were refused (each one aborts operator new, then the loop watchdog resets the base)
	uint32_t sbrkFailures();

} // namespace ram
