#ifndef RESETCAUSE_HPP
#define RESETCAUSE_HPP

#include <cstdint>

// Příčina resetu z RCC->CSR (STM32G431, RM0440 7.4.29)
namespace resetcause {
	// Příznaky spojené '|', např. "PIN|SOFTWARE"; žádný příznak → "NONE". Vrací statický buffer.
	const char *describe(uint32_t csr);
	// Přečte RCC->CSR a smaže příznaky (RMVF); hodnotu si zapamatuje pro last()
	uint32_t readAndClear();
	// Hodnota z posledního readAndClear()
	uint32_t last();
}

#endif
