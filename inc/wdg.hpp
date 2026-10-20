#ifndef WDG_HPP
#define WDG_HPP

#include <cstdint>

// Watchdogy base (spec diagnostiky §3):
// - smyčkový watchdog v SysTick: hlavní smyčka volá kick() jednou za průchod; 1000 ms bez kicku → záznam
//   LOOP_STALL s kontextem přerušeným SysTickem (pc/lr hlavní smyčky) → reset;
// - IWDG ~2 s (LSI 32 kHz, /32, RLR 2000) jako pojistka: zakázaná přerušení, uvízlé ISR, rozbitý SysTick.
// Dlouhé legitimní operace (čekání na RN4870, Oled::flush, DIN odesílání) volají kick() uvnitř svých smyček.
namespace wdg {

	static constexpr uint32_t LOOP_TIMEOUT_MS = 1000;

	void start();
	void kick();
	// Z SysTick_Handler (naked obal předá uložený rámec a EXC_RETURN)
	void onTick(const uint32_t *stackedFrame, uint32_t excReturn);

} // namespace wdg

#endif
