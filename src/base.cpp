#include "base.hpp"
#include "menu.hpp"

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
		gpio_mode_setup(GPIO::PORTA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO::PIN4);
		gpio_set_output_options(GPIO::PORTA, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, GPIO::PIN4);

		//Initialize rotary encoder GPIO
		gpio_mode_setup(GPIO::PORTB, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, GPIO::PIN0 | GPIO::PIN1 | GPIO::PIN2);
	}
}

namespace Base::CurrentSource{
	bool enabled = false;

	//Get actual state of the current source
	bool isEnabled(){
		return !!(gpio_port_read(GPIO::PORTA) & GPIO::PIN4);
	}

	//Enable the current source
	void enable(){
		enabled = true;

		if(!!(gpio_port_read(GPIO::PORTA) & GPIO::PIN4) != enabled){
			gpio_set(GPIO::PORTA, GPIO::PIN4);
		}
	}

	//Disable the current source
	void disable(){
		enabled = false;

		if(!!(gpio_port_read(GPIO::PORTA) & GPIO::PIN4) != enabled){
			gpio_clear(GPIO::PORTA, GPIO::PIN4);
		}
	}

	void toggle(void){
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
		gpioState = gpio_get(GPIO::PORTB, GPIO::PIN1);

		//Dont actually generate any event when turned on for the first time
		if(gpioStateOld == 0xffff){
			gpioStateOld = gpioState;
			return;
		}

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
		if (gpio_get(GPIO::PORTB, GPIO::PIN2)) readCode |= 0x02;
		if (gpio_get(GPIO::PORTB, GPIO::PIN0)) readCode |= 0x01;
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
			GUI::keypress(UserInput::Key::DOWN);
			pos = 0;
		}else if(pos < 0){
			GUI::keypress(UserInput::Key::UP);
			pos = 0;
		}

		if(pressed){
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

