#include "base.hpp"
#include "menu.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <stmcpp/register.hpp>
#include <stmcpp/register_waits.hpp>
#include <stmcpp/gpio.hpp>
#include <stmcpp/scheduler.hpp>

using namespace stmcpp::units;

stmcpp::scheduler::Scheduler keypressScheduler(10_ms, &base::Encoder::process, true, true);

namespace base{

	stmcpp::gpio::pin<stmcpp::gpio::port::porta, 4> current_enable (stmcpp::gpio::mode::output);	
	stmcpp::gpio::pin<stmcpp::gpio::port::portb, 0> encoder_a (stmcpp::gpio::mode::input, stmcpp::gpio::pull::pullUp);
	stmcpp::gpio::pin<stmcpp::gpio::port::portb, 2> encoder_b (stmcpp::gpio::mode::input, stmcpp::gpio::pull::pullUp);
	stmcpp::gpio::pin<stmcpp::gpio::port::portb, 1> encoder_btn (stmcpp::gpio::mode::input, stmcpp::gpio::pull::pullUp);


	//Initialization of necessary objects
	void initClock(){

		// Enable HSI48 and wait for it to be ready
		stmcpp::reg::set(std::ref(RCC->CRRCR), RCC_CRRCR_HSI48ON);
		stmcpp::reg::waitForBitSet(std::ref(RCC->CRRCR), RCC_CRRCR_HSI48RDY, []() { errorHandler.hardThrow(error::hsi48_timeout); });
		
		// Enable HSE and wait for it to be ready
		stmcpp::reg::set(std::ref(RCC->CR), RCC_CR_HSEON);
		stmcpp::reg::waitForBitSet(std::ref(RCC->CR), RCC_CR_HSERDY_Msk, []() { errorHandler.hardThrow(error::hse_timeout); });

		// Set up the flash latency to work with 144MHz
		stmcpp::reg::change(std::ref(FLASH->ACR), FLASH_ACR_LATENCY_Msk, FLASH_ACR_LATENCY_4WS);
		stmcpp::reg::waitForBitsEqual(std::ref(FLASH->ACR), FLASH_ACR_LATENCY_Msk, FLASH_ACR_LATENCY_4WS, []() { errorHandler.hardThrow(error::flash_latency_timeout); });

		// Configure PLL (R = 144 MHz, Q = 48 MHz, P = 144 MHz)
		stmcpp::reg::write(std::ref(RCC->PLLCFGR), 
			    RCC_PLLCFGR_PLLSRC_HSE | 				// source is HSE
				(5 << RCC_PLLCFGR_PLLM_Pos) | 			// divide by 6
				(36 << RCC_PLLCFGR_PLLN_Pos) | 			// multiply by 36
				(0 << RCC_PLLCFGR_PLLR_Pos) | 			// divide by 2
				(2 << RCC_PLLCFGR_PLLQ_Pos) | 			// divide by 6
				//(0b00010 << RCC_PLLCFGR_PLLPDIV_Pos) | 	// divide by 2
				RCC_PLLCFGR_PLLREN |
				RCC_PLLCFGR_PLLQEN 
				//RCC_PLLCFGR_PLLPEN
		);

		// Enable PLL and wait for it to be ready
		stmcpp::reg::set(std::ref(RCC->CR), RCC_CR_PLLON);
		stmcpp::reg::waitForBitSet(std::ref(RCC->CR), RCC_CR_PLLRDY_Msk, []() { errorHandler.hardThrow(error::pll_timeout); });


		// Enable PLL and wait for it to be ready
		stmcpp::reg::set(std::ref(RCC->CFGR), RCC_CFGR_SW_PLL);
		stmcpp::reg::waitForBitsEqual(std::ref(RCC->CFGR ), RCC_CFGR_SWS_Msk, RCC_CFGR_SWS_PLL, []() { errorHandler.hardThrow(error::domain_source_switch_timeout); });

		// Select HSI16 for USART1, this lower clock speed is used to generate the 1200Bd
		// Select PLLQ as 48MHz clock source
		stmcpp::reg::write(std::ref(RCC->CCIPR), 
			(0b10 << RCC_CCIPR_USART1SEL_Pos) |
			(0b10 << RCC_CCIPR_CLK48SEL_Pos)
		);

		
		stmcpp::reg::set(std::ref(RCC->AHB2ENR), 
			RCC_AHB2ENR_GPIOAEN | 
			RCC_AHB2ENR_GPIOBEN |
			RCC_AHB2ENR_GPIOCEN |
			RCC_AHB2ENR_GPIODEN |
			RCC_AHB2ENR_GPIOEEN
		);

		stmcpp::reg::set(std::ref(RCC->APB2ENR), 
			RCC_APB2ENR_USART1EN | 
			RCC_APB2ENR_TIM1EN |
			RCC_APB2ENR_TIM15EN |
			RCC_APB2ENR_TIM17EN
		);


		stmcpp::reg::set(std::ref(RCC->APB1ENR1), 
			RCC_APB1ENR1_USART2EN |
			RCC_APB1ENR1_USART3EN |
			RCC_APB1ENR1_USBEN |
			RCC_APB1ENR1_I2C1EN |
			RCC_APB1ENR1_TIM2EN |
			RCC_APB1ENR1_TIM4EN |
			RCC_APB1ENR1_TIM7EN | 
			RCC_APB1ENR1_RTCAPBEN
		);


		stmcpp::reg::set(std::ref(RCC->AHB1ENR), 
			RCC_AHB1ENR_DMA1EN |
			RCC_AHB1ENR_DMA2EN |
			RCC_AHB1ENR_DMAMUX1EN
		);

	}

	void dfuCheck(){
		// If the encoder isn't pressed
		if(GPIOB->IDR & GPIO_IDR_ID1) return;

		int i = 0;

		while(i < 1000)
		{
			i++;
			if(GPIOB->IDR & GPIO_IDR_ID1) return;
		}
		
		GUI::display(new GUI::Splash("MIDIControl", "DFU MODE", "Program over USB"));
		GUI::render();

		#define FW_ADDR    0x1FFF0000
		SCB->VTOR = FW_ADDR & 0xFFFF;
		__asm__ volatile("msr msp, %0"::"g"(*(volatile uint32_t *)FW_ADDR));
		(*(void (**)())(FW_ADDR + 4))();
	}

	void wdtStart(){
		IWDG->KR = 0x5555; // Enable access to IWDG_PR and IWDG_RLR
		IWDG->PR = 0x06; // Set prescaler to 256
		IWDG->RLR = 2000; // Set reload value
		IWDG->KR = 0xAAAA; // Reload the watchdog
		IWDG->KR = 0xCCCC; // Start the watchdog
	}
} 

namespace base::current{

	// Get actual state of the current source
	bool isEnabled(){
		return current_enable.getIntendedState();
	}

	// Enable the current source
	void enable(){
		current_enable.set();

	}

	// Disable the current source
	void disable(){
		current_enable.clear();
		
	}

	void toggle(void){
		current_enable.toggle();
	}

	void set(bool value){
		current_enable.write(value);
	}

}

namespace base::Encoder{

	

	uint8_t btnPress, btnPressOld = 0xff;

	uint8_t readCode = 0, storedCode = 0;
	int pos = 0;

	// Setup a table of expected states
	static int8_t rotEncTable[] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};


	void process(){

		// Read the state of encoder switch gpio
		btnPress = encoder_btn.read();

		// Don't actually generate any event when turned on for the first time
		if(btnPressOld == 0xff){
			btnPressOld = btnPress;
		} else if(btnPress != btnPressOld){
			// Update the old state
			btnPressOld = btnPress;

			// Update the status according to the gpio state
			if(btnPress){
				GUI::keypress(UserInput::Key::ENTER);
			}
		}
		


		// Read A and B states into code variable
		readCode <<= 2;
		if (encoder_b.read()) readCode |= 0x02;
		if (encoder_a.read()) readCode |= 0x01;
		readCode &= 0x0f;

		// If the code is valid
		if (rotEncTable[readCode]){
			storedCode <<= 4;
			storedCode |= readCode;
			if ((storedCode & 0xff) == 0x2b) pos--;
			if ((storedCode & 0xff) == 0x17) pos++;
		}

		// Handle a shift in position
		if(pos > 0){
			GUI::keypress(UserInput::Key::DOWN);
			pos = 0;
		}else if(pos < 0){
			GUI::keypress(UserInput::Key::UP);
			pos = 0;
		}



	}
}
