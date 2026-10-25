#include "wdg.hpp"
#include "fault.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>

#include "stmcpp/systick.hpp"

// IWDG after a reset (verified on the bench 2026-10-19, STM32G431, IWDG_SW = 1 in FLASH->OPTR): a software-started IWDG
// does NOT survive a system reset (NVIC_SystemReset / SYSRESETREQ) or its own IWDG reset — after a reset PR/RLR have default
// values (0 / 0xFFF) and without being started again it never resets anything. The kicks in the bootloader (Task 6A) are therefore only
// a safeguard (in case of a hardware IWDG in the option bytes); the application starts the IWDG itself on every boot (start()).

namespace wdg {

	static volatile uint32_t _sinceKick = 0; // ms since the last kick()
	static volatile bool	 _armed		= false;

	void start() {
		// The IWDG stops while the core is halted by the probe (otherwise a gdb session would reset the base)
		DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;

		// RM0440 IWDG: start first (turns on the LSI), then unlock PR/RLR and wait for them to be taken over
		IWDG->KR	   = 0xCCCC;
		IWDG->KR	   = 0x5555;
		IWDG->PR	   = 3; // /32 → 1 kHz from the 32 kHz LSI
		IWDG->RLR	   = 2000; // 2000 ticks = ~2 s (LSI ±: roughly 1.9–2.2 s)
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
