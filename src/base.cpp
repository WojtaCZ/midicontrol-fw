#include "base.hpp"
#include "oled.hpp"
#include "menu.hpp"


#include <string>
#include <stm32/exti.h>
#include <stm32/gpio.h>
#include <stm32/rcc.h>
#include <stm32/usart.h>
#include <stm32/flash.h>
#include <stm32/timer.h>
#include <stm32/iwdg.h>
#include <cm3/nvic.h>
#include <cm3/systick.h>
#include <cm3/assert.h>
#include <cm3/scb.h>

Scheduler keypressScheduler(10, &Base::Encoder::process, Scheduler::PERIODICAL | Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);

namespace Base{
	//Initialization of necessary objects
	void init(){
		//Create structure to define clock setup
		const struct rcc_clock_scale rcc_hse_48mhz_3v3[] = {
				{ /* 144MHz */
					.pllm = 6,
					.plln = 36,
					.pllp = 2,
					.pllq = 6,
					.pllr = 2,
					.pll_source = RCC_PLLCFGR_PLLSRC_HSE,
					.hpre = RCC_CFGR_HPRE_NODIV,
					.ppre1 = RCC_CFGR_PPREx_NODIV,
					.ppre2 = RCC_CFGR_PPREx_NODIV,
					.vos_scale = PWR_SCALE1,
					.boost = true,
					.flash_config = FLASH_ACR_DCEN | FLASH_ACR_ICEN,
					.flash_waitstates = 6,
					.ahb_frequency  = 144000000,
					.apb1_frequency = 144000000,
					.apb2_frequency = 144000000,
				}
			};

		//Setup the PLL
		rcc_clock_setup_pll(&rcc_hse_48mhz_3v3[0]);

		rcc_wait_for_osc_ready(RCC_HSE);

		//Enable LSE
		pwr_disable_backup_domain_write_protect();
		rcc_osc_on(RCC_LSE);
		rcc_wait_for_osc_ready(RCC_LSE);
		pwr_enable_backup_domain_write_protect();

		//Enable clock for peripherals
		rcc_periph_clock_enable(RCC_GPIOA);
		rcc_periph_clock_enable(RCC_GPIOB);
		rcc_periph_clock_enable(RCC_GPIOC);
		rcc_periph_clock_enable(RCC_GPIOD);
		rcc_periph_clock_enable(RCC_GPIOE);
		rcc_periph_clock_enable(RCC_USART1);
		rcc_periph_clock_enable(RCC_USART2);
		rcc_periph_clock_enable(RCC_USART3);
		rcc_periph_clock_enable(RCC_USB);
		rcc_periph_clock_enable(RCC_I2C1);
		rcc_periph_clock_enable(RCC_TIM1);
		rcc_periph_clock_enable(RCC_TIM2);
		rcc_periph_clock_enable(RCC_TIM4);
		rcc_periph_clock_enable(RCC_TIM7);
		rcc_periph_clock_enable(RCC_TIM15);
		rcc_periph_clock_enable(RCC_TIM17);
		rcc_periph_clock_enable(RCC_RTCAPB);
		rcc_periph_clock_enable(RCC_SYSCFG);
		rcc_periph_clock_enable(RCC_DMA1);
		rcc_periph_clock_enable(RCC_DMA2);
		//rcc_periph_clock_enable(RCC_PWR);
		//rcc_periph_clock_enable(RCC_WWDG);
		//rcc_periph_clock_enable(RCC_CRC);
		//rcc_periph_clock_enable(RCC_SAI1);
		rcc_periph_clock_enable(RCC_DMAMUX1);

		//Initialize and enable systick timer
		systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
		systick_set_reload(143999);
		systick_interrupt_enable();
		systick_clear();
		systick_counter_enable();

		//Initialize current source GPIO
		gpio_mode_setup(Gpio::PORT_CURRENT_SOURCE, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, Gpio::GPIO_CURRENT_SOURCE);
		gpio_set_output_options(Gpio::PORT_CURRENT_SOURCE, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, Gpio::GPIO_CURRENT_SOURCE);

		//Initialize rotary encoder GPIO
		gpio_mode_setup(Gpio::PORT_ENCODER, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, Gpio::GPIO_ENCODER_A | Gpio::GPIO_ENCODER_B | Gpio::GPIO_ENCODER_SW);
	}
}

namespace Base::CurrentSource{
	bool enabled = false;

	//Get actual state of the current source
	bool isEnabled(){
		return !!(gpio_port_read(Gpio::PORT_CURRENT_SOURCE) & Gpio::GPIO_CURRENT_SOURCE);
	}

	//Enable the current source
	void enable(){
		enabled = true;

		if(!!(gpio_port_read(Gpio::PORT_CURRENT_SOURCE) & Gpio::GPIO_CURRENT_SOURCE) != enabled){
			gpio_set(Gpio::PORT_CURRENT_SOURCE, Gpio::GPIO_CURRENT_SOURCE);
		}
	}

	//Disable the current source
	void disable(){
		enabled = false;

		if(!!(gpio_port_read(Gpio::PORT_CURRENT_SOURCE) & Gpio::GPIO_CURRENT_SOURCE) != enabled){
			gpio_clear(Gpio::PORT_CURRENT_SOURCE, Gpio::GPIO_CURRENT_SOURCE);
		}
	}

	void toggle(){
		if(isEnabled()){
			disable();
		} else enable();
	}

}

namespace Base::Encoder{
	uint16_t gpioState, gpioStateOld = 0xffff;
    uint8_t readCode = 0, storedCode = 0;
    int pos = 0;
    bool pressed;

	void process(){

		//Read the state of encoder switch gpio
		gpioState = gpio_get(Gpio::PORT_ENCODER, Gpio::GPIO_ENCODER_SW);
		//If the state differs from the previous one
		if(gpioState != gpioStateOld){
			//Update the old state
			gpioStateOld = gpioState;

			//Update the status according to the gpio state
			if(gpioState){
				pressed = true;
			}else pressed = false;
		}
		
		//Setup a table of expected states
		static int8_t rotEncTable[] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};

		//Read A and B states into code variable
		readCode <<= 2;
		if (gpio_get(Gpio::PORT_ENCODER, Gpio::GPIO_ENCODER_B)) readCode |= 0x02;
		if (gpio_get(Gpio::PORT_ENCODER, Gpio::GPIO_ENCODER_A)) readCode |= 0x01;
		readCode &= 0x0f;

		//If the code is valid
		if (rotEncTable[readCode]){
			storedCode <<= 4;
			storedCode |= readCode;
			if ((storedCode & 0xff) == 0x2b) pos--;
			if ((storedCode & 0xff) == 0x17) pos++;
		}

		//Handle a shift in position
		if(pos > 0){
			Oled::wakeupCallback();
			GUI::keypress(UserInput::Key::DOWN);
			pos = 0;
		}else if(pos < 0){
			Oled::wakeupCallback();
			GUI::keypress(UserInput::Key::UP);
			pos = 0;
		}

		if(pressed){
			Oled::wakeupCallback();
			GUI::keypress(UserInput::Key::ENTER);
			pressed = false;

		}

	}
}




//Some bootloader magic happening here
/*
void bootloader_check(){
	#define FW_ADDR    0x1FFF0000

	SCB_VTOR = FW_ADDR & 0xFFFF;

	__asm__ volatile("msr msp, %0"::"g"(*(volatile uint32_t *)FW_ADDR));

	(*(void (**)())(FW_ADDR + 4))();
}*/




/////////////////////////////////////////////////////////////////////////////////
////////////////// CODE TO BE REWRITTEN

/*
void base_req_songlist(){
	comm_cmd_send(CMD_GET | MUSIC_SONGLIST , "");
}


//Rutina pro spusteni nahravani
void base_record(uint8_t initiator, char * songname){
	char buffer[] = "set record";
    usb_cdc_tx(buffer, strlen(buffer));

	//Spusteno z PC
	if(initiator == ADDRESS_PC){
		//Jen se zobrazi obrazovka nahravani
		base_set_current_source(1);
		//oled_setDisplayedSplash(oled_recordingSplash, songname);
		
	}else if(initiator == ADDRESS_CONTROLLER){
	//Spusteno ovladacem
		//Jen se zobrazi obrazovka nahravani
		//oled_setDisplayedSplash(oled_recordingSplash, songname);
		//oled_refreshPause();
	}else if(initiator == ADDRESS_MAIN){
	//Spusteno ze zakladnove stanice
		//Posle se zprava do PC aby zacalo nahravat
		char msg[100];
		msg[0] = INTERNAL_COM;
		msg[1] = INTERNAL_COM_REC;
		memcpy(&msg[2], songname, strlen(songname));
		comm_send_msg(ADDRESS_MAIN, ADDRESS_PC, 0, INTERNAL, msg, strlen(songname)+2);
	}


//}

void base_play(Menu * menu){
	//comm_cmd_send(CMD_SET | MUSIC_PLAY, menu->items[menu->selectedIndex]->text[0]);
	char buffer[] = "set play";
    usb_cdc_tx(buffer, strlen(buffer));
	//Spusteno z PC
	if(initiator == ADDRESS_PC){
		base_set_current_source(1);
		//memset(selectedSong, 0, 40);
		//sprintf(selectedSong, "%s", songname);
		//Jen se zobrazi obrazovka prehravani
		//oled_setDisplayedSplash(oled_playingSplash, songname);
		//oled_refreshPause();
	}else if(initiator == ADDRESS_CONTROLLER){
	//Spusteno ovladacem
		//Jen se zobrazi obrazovka prehravani
		//oled_setDisplayedSplash(oled_playingSplash, songname);
		//oled_refreshPause();
	}else if(initiator == ADDRESS_MAIN){
	//Spusteno ze zakladnove stanice
		//Posle se zprava do PC aby zacalo prehravat
		char msg[100];
		msg[0] = INTERNAL_COM;
		msg[1] = INTERNAL_COM_PLAY;
		memcpy(&msg[2], songname, strlen(songname));
		comm_send_msg(ADDRESS_MAIN, ADDRESS_PC, 0, INTERNAL, msg, strlen(songname)+2);
	}


}

void base_stop(uint8_t initiator){
	//Spusteno z hlavni jednotky
	if(initiator == ADDRESS_MAIN){
		//Posle se zprava do PC o zastaveni
		char msg[2] = {INTERNAL_COM, INTERNAL_COM_STOP};
		comm_send_msg(ADDRESS_MAIN, ADDRESS_PC, 0, INTERNAL, msg, 2);
	}else{
		//Vrati se do menu, zapne OLED refresh a vypne LED
		//oledType = OLED_MENU;
		//oled_refreshResume();
		//setStatusAll(1, DEV_CLR);
	}

}*/