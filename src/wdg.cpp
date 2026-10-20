#include "wdg.hpp"
#include "fault.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>

#include "stmcpp/systick.hpp"

// IWDG po resetu (ověřeno na lavici 2026-10-19, STM32G431, IWDG_SW = 1 v FLASH->OPTR): softwarově spuštěný IWDG
// NEpřežije systémový reset (NVIC_SystemReset / SYSRESETREQ) ani vlastní IWDG reset — po resetu má PR/RLR výchozí
// hodnoty (0 / 0xFFF) a bez nového spuštění už nikdy nezresetuje. Kicky v bootloaderu (Task 6A) jsou proto jen
// pojistka (pro případ hardwarového IWDG v option bytes); aplikace IWDG spouští při každém bootu sama (start()).

namespace wdg {

	static volatile uint32_t _sinceKick = 0; // ms od posledního kick()
	static volatile bool	 _armed		= false;

	void start() {
		// IWDG stojí, když je jádro zastavené sondou (jinak by gdb session base resetovala)
		DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;

		// RM0440 IWDG: nejdřív spustit (zapne LSI), pak odemknout PR/RLR a počkat na jejich převzetí
		IWDG->KR	   = 0xCCCC;
		IWDG->KR	   = 0x5555;
		IWDG->PR	   = 3; // /32 → 1 kHz z LSI 32 kHz
		IWDG->RLR	   = 2000; // 2000 tiků = ~2 s (LSI ±: zhruba 1,9–2,2 s)
		uint32_t start = stmcpp::systick::getTicks();
		while (IWDG->SR && (stmcpp::systick::getTicks() - start) < 50) {}
		IWDG->KR = 0xAAAA;

		_sinceKick = 0;
		_armed	   = true;
	}

	void kick() {
		IWDG->KR   = 0xAAAA;
		_sinceKick = 0;
	}

	void onTick(const uint32_t *stackedFrame, uint32_t excReturn) {
		if (!_armed) return;
		uint32_t n = _sinceKick + 1;
		_sinceKick = n;
		if (n >= LOOP_TIMEOUT_MS) {
			_armed = false;
			fault::capture(fault::Kind::LOOP_STALL, stackedFrame, excReturn);
		}
	}

} // namespace wdg
