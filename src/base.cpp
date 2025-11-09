#include "base.hpp"
#include "menu.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

Scheduler keypressScheduler(10, &Base::Encoder::process, Scheduler::PERIODICAL | Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);

namespace Base{
	
	//Initialization of necessary objects
	void init(){

		// Enable HSI
		RCC->CR |= RCC_CR_HSION;
		while (!(RCC->CR & RCC_CR_HSIRDY));

		// Enable HSE
		RCC->CR |= RCC_CR_HSEON;
		while (!(RCC->CR & RCC_CR_HSERDY));

		// Enable HSI48
		RCC->CRRCR |= RCC_CRRCR_HSI48ON;
		while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY));
		

		// Enable PWR clock
		RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
		// Set voltage scaling to scale 1
		PWR->CR1 = (PWR->CR1 & ~PWR_CR1_VOS_Msk) | (1 << PWR_CR1_VOS_Pos);
		// Enable boost mode
		PWR->CR5 |= PWR_CR5_R1MODE;

		// Configure PLL
		RCC->PLLCFGR =       RCC_PLLCFGR_PLLSRC_HSE | 
						(5 << RCC_PLLCFGR_PLLM_Pos) | // divide by 6
						(36 << RCC_PLLCFGR_PLLN_Pos) | 
						(0 << RCC_PLLCFGR_PLLPDIV_Pos) | 
						(2 << RCC_PLLCFGR_PLLQ_Pos) | // divide by 6
						(0 << RCC_PLLCFGR_PLLR_Pos) | 
						RCC_PLLCFGR_PLLREN | RCC_PLLCFGR_PLLQEN | RCC_PLLCFGR_PLLPEN;
		RCC->CR |= RCC_CR_PLLON;
		while (!(RCC->CR & RCC_CR_PLLRDY));

		// Set flash latency
		FLASH->ACR |= FLASH_ACR_LATENCY_4WS;

		// Select PLL as system clock source
		RCC->CFGR |= RCC_CFGR_SW_PLL;
		while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);

		// Enable LSE
		PWR->CR1 |= PWR_CR1_DBP; // Enable access to backup domain
		RCC->BDCR |= RCC_BDCR_LSEON;
		while (!(RCC->BDCR & RCC_BDCR_LSERDY));
		PWR->CR1 &= ~PWR_CR1_DBP; // Disable access to backup domain

		// Enable LSI
		RCC->CSR |= RCC_CSR_LSION;
		while (!(RCC->CSR & RCC_CSR_LSIRDY));

		RCC->CCIPR |= (0b10 << RCC_CCIPR_USART1SEL_Pos); // Select HSI16 for USART1

		// Enable peripheral clocks
		RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN | RCC_AHB2ENR_GPIODEN | RCC_AHB2ENR_GPIOEEN;
		RCC->APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_TIM1EN | RCC_APB2ENR_TIM15EN | RCC_APB2ENR_TIM17EN;
		RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN | RCC_APB1ENR1_USART3EN | RCC_APB1ENR1_USBEN | RCC_APB1ENR1_I2C1EN | RCC_APB1ENR1_TIM2EN | RCC_APB1ENR1_TIM4EN | RCC_APB1ENR1_TIM7EN | RCC_APB1ENR1_RTCAPBEN;
		RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMA2EN | RCC_AHB1ENR_DMAMUX1EN;

		// Initialize and enable systick timer
		/*SysTick->LOAD = 143999;
		SysTick->VAL = 0;
		SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;*/

		// Initialize current source GPIO
		GPIOA->MODER = (GPIOA->MODER & ~(GPIO_MODER_MODE4_Msk)) | (GPIO_MODER_MODE4_0);
		GPIOA->OTYPER &= ~GPIO_OTYPER_OT4;
		GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(GPIO_OSPEEDR_OSPEED4_Msk)) | (GPIO_OSPEEDR_OSPEED4_1);

		// Initialize rotary encoder GPIO
		GPIOB->MODER &= ~(GPIO_MODER_MODE0_Msk | GPIO_MODER_MODE1_Msk | GPIO_MODER_MODE2_Msk);
		GPIOB->PUPDR = (GPIOB->PUPDR & ~(GPIO_PUPDR_PUPD0_Msk | GPIO_PUPDR_PUPD1_Msk | GPIO_PUPDR_PUPD2_Msk)) | (GPIO_PUPDR_PUPD0_0 | GPIO_PUPDR_PUPD1_0 | GPIO_PUPDR_PUPD2_0);
		
		

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

namespace Base::CurrentSource{
	bool enabled = false;

	// Get actual state of the current source
	bool isEnabled(){
		return !!(GPIOA->ODR & GPIO_ODR_OD4);
	}

	// Enable the current source
	void enable(){
		enabled = true;

		if(!!(GPIOA->ODR & GPIO_ODR_OD4) != enabled){
			GPIOA->BSRR = GPIO_BSRR_BS4;
		}
	}

	// Disable the current source
	void disable(){
		enabled = false;

		if(!!(GPIOA->ODR & GPIO_ODR_OD4) != enabled){
			GPIOA->BSRR = GPIO_BSRR_BR4;
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

		// Read the state of encoder switch gpio
		gpioState = GPIOB->IDR & GPIO_IDR_ID1;

		// Don't actually generate any event when turned on for the first time
		if(gpioStateOld == 0xffff){
			gpioStateOld = gpioState;
			return;
		}

		// If the state differs from the previous one
		if(gpioState != gpioStateOld){
			// Update the old state
			gpioStateOld = gpioState;

			// Update the status according to the gpio state
			if(gpioState){
				pressed = true;
			}else pressed = false;
		}
		
		// Setup a table of expected states
		static int8_t rotEncTable[] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};

		// Read A and B states into code variable
		readCode <<= 2;
		if (GPIOB->IDR & GPIO_IDR_ID2) readCode |= 0x02;
		if (GPIOB->IDR & GPIO_IDR_ID0) readCode |= 0x01;
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

		if(pressed){
			GUI::keypress(UserInput::Key::ENTER);
			pressed = false;
		}

	}
}
