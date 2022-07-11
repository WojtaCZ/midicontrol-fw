#include "main.hpp"
#include "oled.hpp"
#include "base.hpp"
#include "scheduler.hpp"
#include "menu.hpp"
#include "midi.hpp"
#include "led.hpp"

/*#include <scheduler.hpp<*//*
#include <led.hpp<
#include <usb.h>
#include <midi.hpp<
#include <ble.hpp<*/
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

/*extern Menu::Menu menu_main;

Menu::Renderer rndr(true);*/

extern Scheduler oledSleepScheduler;
extern Scheduler keypressScheduler;
extern Scheduler keepaliveScheduler;
extern Scheduler guiRenderScheduler;
extern Scheduler menuScrollScheduler;


extern "C" void usb_init();

extern "C" void SystemInit(void) {
	//Initialize io and other stuff related to the base unit
	Base::init();
	//Initialize the OLED
	Oled::init();
	//Initialize MIDI
	//MIDI::init();
	//Initialize USB
	usb_init();

	//Initialize LED indicators
	LED::init();
}

extern "C" void SysTick_Handler(void){
	//menuScroll.check();
	//menuRender.check();
	//ioKeypress.check();
	//oledSleep.check();
	//commTimeout.check();
	//ledProcess.check();
	//keepaliveScheduler.increment();
	oledSleepScheduler.increment();
	keypressScheduler.increment();
	guiRenderScheduler.increment();
	menuScrollScheduler.increment();
	//Process inputs 
}


extern "C" int main(void)
{
	//Base::init();
	/*gpio_mode_setup(PORT_CURRENT_SOURCE, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_CURRENT_SOURCE);
    gpio_set_output_options(PORT_CURRENT_SOURCE, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, GPIO_CURRENT_SOURCE);
	gpio_set(PORT_CURRENT_SOURCE, GPIO_CURRENT_SOURCE);
	*/
	//Pauza pro startup (zmizi zakmity na PSU)
	/*int i = 0;
	while(i < 1000000){
		__asm__(<nop<);
		i++;
	}*/



	/*usb_init();

	midi_init();

	ble_init();*/


	//led_init();

	/*led_dev_set_status(LED_DEV_USB, LED_STATUS_OK);.
	led_dev_process_pending_status();*/

	//rndr.display(menu_main);
	//rndr.render();
	

	LED::USB.setColor(LED::Color(0xff, 0x00, 0xff));
	

	while (1) {
		int i = 0;
	while(i < 1000000){
		__asm__("nop");
		i++;
	}
		//menuScroll.process();
		//menuRender.process();
		//ioKeypress.process();
		//oledSleep.process();
		//commTimeout.process();
		//ledProcess.process();
	}

	return 0;
}



extern "C" void HardFault_Handler(void) {
	while(1){
		__asm__("nop");
	}
}
