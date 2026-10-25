#ifndef RESETCAUSE_HPP
#define RESETCAUSE_HPP

#include <cstdint>

// Reset cause from RCC->CSR (STM32G431, RM0440 7.4.29)
namespace resetcause {
	// Flags joined by '|', e.g. "PIN|SOFTWARE"; no flag → "NONE". Returns a static buffer.
	const char *describe(uint32_t csr);
	// Reads RCC->CSR and clears the flags (RMVF); remembers the value for last()
	uint32_t readAndClear();
	// Value from the last readAndClear()
	uint32_t last();
}

#endif
