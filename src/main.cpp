#include "main.hpp"
#include "oled.hpp"
#include "base.hpp"
#include "scheduler.hpp"
#include "menu.hpp"
#include "midi.hpp"
#include "led.hpp"
#include "ble.hpp"
#include "display.hpp"

#include <utility>

#include <stm32/exti.h>
#include <stm32/gpio.h>
#include <stm32/rcc.h>
#include <stm32/usart.h>
#include <stm32/flash.h>
#include <stm32/iwdg.h>

#include <cm3/nvic.h>
#include <cm3/systick.h>
#include <cm3/assert.h>

extern Scheduler oledSleepScheduler;
extern Scheduler keypressScheduler;
extern Scheduler keepaliveScheduler;
extern Scheduler guiRenderScheduler;
extern Scheduler menuScrollScheduler;

Scheduler startupSplashScheduler(2000, [](void){GUI::displayActiveMenu(); oledSleepScheduler.resume();}, Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);




extern "C" void usb_init();

extern "C" void SystemInit(void) {
	//Initialize io and other stuff related to the base unit
	Base::init();
	//Initialize the OLED
	Oled::init();
	//Initialize MIDI
	MIDI::init();
	//Initialize USB
	usb_init();
	//Initialize LED indicators
	LED::init();
	//Initialize bluetooth
	BLE::init();
	//Initialize LED display
	Display::init();

	oledSleepScheduler.pause();

}

extern "C" void SysTick_Handler(void){
	//commTimeout.check();
	//keepaliveScheduler.increment();
	oledSleepScheduler.increment();
	keypressScheduler.increment();
	guiRenderScheduler.increment();
	menuScrollScheduler.increment();
	startupSplashScheduler.increment();
	//Process inputs 
}


extern "C" int main(void)
{
	
	Display::send({1, 2, 3, 4, 5, 6, 7, 8, 9});

	while (1) {
		int i = 0;
		while(i < 1000000){
			__asm__("nop");
			i++;
		}

		

	}

	return 0;
}



extern "C" void HardFault_Handler(void) {
	Base::CurrentSource::disable();
	while(1){
		__asm__("nop");
	}
}
