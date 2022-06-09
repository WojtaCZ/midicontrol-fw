#include "base.hpp"
#include "menu.hpp"
#include "comm.hpp"
#include "usb.h"

#include <string.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/assert.h>
#include <libopencm3/cm3/scb.h>


using namespace std;


//Get actual state of the current source
bool Base::CurrentSource::isEnabled(){
	return !!(gpio_port_read(PORT_CURRENT_SOURCE) & GPIO_CURRENT_SOURCE);
}

//Enable the current source
void Base::CurrentSource::enable(){
	this->enabled = true;

	if(!!(gpio_port_read(PORT_CURRENT_SOURCE) & GPIO_CURRENT_SOURCE) != this->enabled){
        gpio_set(PORT_CURRENT_SOURCE, GPIO_CURRENT_SOURCE);
    }
}

//Disable the current source
void Base::CurrentSource::disable(){
	this->enabled = false;

	if(!!(gpio_port_read(PORT_CURRENT_SOURCE) & GPIO_CURRENT_SOURCE) != this->enabled){
        gpio_clear(PORT_CURRENT_SOURCE, GPIO_CURRENT_SOURCE);
    }
}

//Initialization of necessary objects
void Base::init(){
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
    gpio_mode_setup(PORT_CURRENT_SOURCE, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_CURRENT_SOURCE);
    gpio_set_output_options(PORT_CURRENT_SOURCE, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, GPIO_CURRENT_SOURCE);

	//Initialize rotary encoder GPIO
	gpio_mode_setup(PORT_ENCODER, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, GPIO_ENCODER_A | GPIO_ENCODER_B | GPIO_ENCODER_SW);
}

void Base::Encoder::process(){

	//Read the state of encoder switch gpio
	this->gpioState = gpio_get(PORT_ENCODER, GPIO_ENCODER_SW);
	//If the state differs from the previous one
    if(this->gpioState != this->gpioStateOld){
		//Update the old state
        this->gpioStateOld = this->gpioState;

		//Update the status according to the gpio state
        if(this->gpioState){
			this->pressed = true;
		}else this->pressed = false;
    }
	
	//Setup a table of expected states
	static int8_t rotEncTable[] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};

	//Read A and B states into code variable
    this->readCode <<= 2;
    if (gpio_get(PORT_ENCODER, GPIO_ENCODER_B)) this->readCode |= 0x02;
    if (gpio_get(PORT_ENCODER, GPIO_ENCODER_A)) this->readCode |= 0x01;
    this->readCode &= 0x0f;

    //If the code is valid
    if (rotEncTable[readCode]){
        this->storedCode <<= 4;
        this->storedCode |= this->readCode;
        if ((this->storedCode & 0xff) == 0x2b) pos--;
        if ((this->storedCode & 0xff) == 0x17) pos++;
    }

	//Handle a shift in position
	if(this->pos > 0){

		//OLED wakeup
	}else if(this->pos < 0){

		//OLED wakeup
	}

	if(this->pressed){

		//OLED wakeup
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