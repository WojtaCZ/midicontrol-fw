#include "main.hpp"
#include "oled.hpp"
#include "menu.hpp"
#include "base.hpp"
#include "scheduler.hpp"/*
#include "led.hpp"
#include "usb.h"
#include "midi.hpp"
#include "ble.hpp"*/

#include <utility>

#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/iwdg.h>

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/assert.h>

extern Menu::Menu menu_main;
extern Oled::Oled oled;
Menu::Renderer rndr(true);


extern "C" void SystemInit(void) {
	//Initialize io and other stuff related to the base unit
	Base::init();
	//Initialize objects related to the OLED
	Oled::init();
}

extern "C" void SysTick_Handler(void){
	//menuScroll.check();
	//menuRender.check();
	//ioKeypress.check();
	//oledSleep.check();
	//commTimeout.check();
	//ledProcess.check();
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
		__asm__("nop");
		i++;
	}*/



	/*usb_init();

	midi_init();

	ble_init();*/


	//led_init();

	/*led_dev_set_status(LED_DEV_USB, LED_STATUS_OK);.
	led_dev_process_pending_status();*/

	//rndr.display(menu_main);
	rndr.render();
	

	//led_dev_set_status_all(LED_STRIP_BACK, LED_STATUS_LOAD);
	//led_dev_set_status_all(LED_STRIP_FRONT, LED_STATUS_LOAD);

	while (1) {
		oled.update();
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

