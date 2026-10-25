#ifndef WDG_HPP
#define WDG_HPP

#include <cstdint>

// Base watchdogs (diagnostics spec §3):
// - loop watchdog in SysTick: the main loop calls kick() once per iteration; 1000 ms without a kick → a LOOP_STALL
//   record with the context interrupted by SysTick (pc/lr of the main loop) → reset;
// - IWDG ~2 s (LSI 32 kHz, /32, RLR 2000) as a safety net: disabled interrupts, a stuck ISR, a broken SysTick.
// Long legitimate operations (waiting for the RN4870, Oled::flush, DIN sending) call kick() inside their loops.
namespace wdg {

	static constexpr uint32_t LOOP_TIMEOUT_MS = 1000;

	void start();
	void kick();
	// From SysTick_Handler (the naked wrapper passes the saved frame and EXC_RETURN)
	void onTick(const uint32_t *stackedFrame, uint32_t excReturn);

} // namespace wdg

#endif
