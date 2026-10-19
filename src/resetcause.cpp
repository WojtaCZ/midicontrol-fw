#include "resetcause.hpp"

#include <cstring>

#include <stm32g431xx.h>

namespace resetcause {

	static uint32_t _last = 0;

	const char *describe(uint32_t csr) {
		static const struct {
			uint32_t bit;
			const char *name;
		} flags[] = {
			{RCC_CSR_LPWRRSTF, "LOWPOWER"}, {RCC_CSR_WWDGRSTF, "WWDG"}, {RCC_CSR_IWDGRSTF, "IWDG"}, {RCC_CSR_SFTRSTF, "SOFTWARE"},
			{RCC_CSR_BORRSTF, "BOR"},       {RCC_CSR_PINRSTF, "PIN"},   {RCC_CSR_OBLRSTF, "OBL"},
		};
		static char text[48];
		text[0] = '\0';
		for (const auto &f : flags) {
			if (!(csr & f.bit)) continue;
			if (text[0]) strcat(text, "|");
			strcat(text, f.name);
		}
		if (!text[0]) strcpy(text, "NONE");
		return text;
	}

	uint32_t readAndClear() {
		_last = RCC->CSR;
		RCC->CSR |= RCC_CSR_RMVF; // smazat příznaky, ať další boot ukáže jen svou příčinu
		return _last;
	}

	uint32_t last() { return _last; }

}
