#include "led.hpp"
#include "base.hpp"

#include <core_cm4.h>
#include <cmsis_compiler.h>
#include  <stm32g431xx.h>

namespace LED{

	Peripheral usb(LedID::usb);
	Peripheral Display(LedID::display);
	Peripheral Current(LedID::current);
	Peripheral MIDIA(LedID::midia);
	Peripheral MIDIB(LedID::midib);
	Peripheral Bluetooth(LedID::bluetooth);

	uint8_t ledFrontBuffer[LED_FRONT_BUFFER_SIZE];
	uint8_t ledBackBuffer[LED_BACK_BUFFER_SIZE];

	uint32_t led_pending_flags[LED_BACK_NUMBER];
	uint32_t led_statuses[LED_BACK_NUMBER];

	void init(){

		// GPIO setup
		RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN; // Enable GPIOB clock

		GPIOB->MODER = (GPIOB->MODER & ~(GPIO_MODER_MODE14_Msk | GPIO_MODER_MODE5_Msk)) | (GPIO_MODER_MODE14_1 | GPIO_MODER_MODE5_1); // Alternate function mode
		GPIOB->OTYPER &= ~(GPIO_OTYPER_OT14 | GPIO_OTYPER_OT5); // Push-pull
		GPIOB->OSPEEDR |= (GPIO_OSPEEDR_OSPEED14 | GPIO_OSPEEDR_OSPEED5); // High speed
		GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(GPIO_AFRH_AFSEL14_Msk)) | (1 << GPIO_AFRH_AFSEL14_Pos); // AF1 for TIM17_CH1
		GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(GPIO_AFRL_AFSEL5_Msk)) | (10 << GPIO_AFRL_AFSEL5_Pos); // AF10 for TIM15_CH1

		// DMA setup for back LED
		RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN; // Enable DMA2 clock
		RCC->AHB1ENR |= RCC_AHB1ENR_DMAMUX1EN; // Enable DMAMUX1 clock

		DMA2_Channel1->CCR = DMA_CCR_PL_0 | DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_DIR; // Low priority, memory increment, circular mode, read from memory
		DMAMUX1_Channel0->CCR = 0x47; // TIM17_CH1 request

		DMA2_Channel1->CPAR = (uint32_t)&TIM17->CCR1;
		DMA2_Channel1->CMAR = (uint32_t)ledBackBuffer;
		DMA2_Channel1->CNDTR = LED_BACK_BUFFER_SIZE;

		// Timer setup for back LED
		RCC->APB2ENR |= RCC_APB2ENR_TIM17EN; // Enable TIM17 clock

		TIM17->CR1 = TIM_CR1_ARPE; // Auto-reload preload enable
		TIM17->DIER = TIM_DIER_UDE; // DMA request on update event
		TIM17->EGR = TIM_EGR_UG; // Generate an update event
		TIM17->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1; // PWM mode 1
		TIM17->CCER = TIM_CCER_CC1E; // Enable output
		TIM17->BDTR = TIM_BDTR_MOE; // Main output enable
		TIM17->ARR = LED_TIMER_PERIOD - 1;

		TIM17->CR1 |= TIM_CR1_CEN; // Enable counter
		DMA2_Channel1->CCR |= DMA_CCR_EN; // Enable DMA channel

		// DMA setup for front LED
		DMA2_Channel2->CCR = DMA_CCR_PL_0 | DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_DIR; // Low priority, memory increment, circular mode, read from memory
		DMAMUX1_Channel1->CCR = 0x46; // TIM15_CH1 request

		DMA2_Channel2->CPAR = (uint32_t)&TIM15->CCR1;
		DMA2_Channel2->CMAR = (uint32_t)ledFrontBuffer;
		DMA2_Channel2->CNDTR = LED_FRONT_BUFFER_SIZE;

		// Timer setup for front LED
		RCC->APB2ENR |= RCC_APB2ENR_TIM15EN; // Enable TIM15 clock

		TIM15->CR1 = TIM_CR1_ARPE; // Auto-reload preload enable
		TIM15->DIER = TIM_DIER_UDE; // DMA request on update event
		TIM15->EGR = TIM_EGR_UG; // Generate an update event
		TIM15->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1; // PWM mode 1
		TIM15->CCER = TIM_CCER_CC1E; // Enable output
		TIM15->BDTR = TIM_BDTR_MOE; // Main output enable
		TIM15->ARR = LED_TIMER_PERIOD - 1;

		TIM15->CR1 |= TIM_CR1_CEN; // Enable counter
		DMA2_Channel2->CCR |= DMA_CCR_EN; // Enable DMA channel
	}

	//Nastaveni barvy LED podle statusu
	void dev_set_status(uint8_t perif, uint8_t status){
		uint32_t color;

		switch(status){
			case LED_STATUS_ERR:
				color = LED_CLR_ERROR;
			break;

			case LED_STATUS_OK:
				color = LED_CLR_OK;
			break;

			case LED_STATUS_DATA:
				color = LED_CLR_DATA;
			break;

			case LED_STATUS_LOAD:
				color = LED_CLR_LOAD;
			break;

			default:
				color = 0;

		}

		//Pokude je typu OK, ERR nebo LOAD - nastavi se na pevne sviceni
		if(status == LED_STATUS_OK || status == LED_STATUS_ERR || status == LED_STATUS_LOAD){
			led_statuses[perif] = color;
			set_color((perif >> 7) & 0x01, perif & 0x7f, ((led_pending_flags[perif]>>16) & 0xff), ((led_pending_flags[perif]>>8) & 0xff), (led_pending_flags[perif] & 0xff));
		}else if(status == LED_STATUS_DATA){
			//Pokud jde o data, udela se jen probliknuti
			led_pending_flags[perif] = color;
		}
	}


	//Nastaveni barvy LED podle statusu
	void dev_set_color(uint8_t perif, uint32_t color){

		led_statuses[perif] = color;
		set_color((perif >> 7) & 0x01, perif & 0x7f, ((led_pending_flags[perif]>>16) & 0xff), ((led_pending_flags[perif]>>8) & 0xff), (led_pending_flags[perif] & 0xff));
	}


	//Rutina pro nastaveni barvy vsech LED najednou
	void dev_set_color_all(uint8_t strip, uint32_t color){

		//Rozdeli se na dva "pasky" podle nazvu
		for(uint16_t i = 0; i < PERIF_COUNT; i++){
			if(((i & 0x80)>>7) == strip) led_statuses[i] = color;
		}

		//Nastavi se barva
		set_strip_color(strip, ((color>>16) & 0xff), ((color>>8) & 0xff), (color & 0xff));

	}


	//Rutina pro nastaveni barvy vsech LED najednou
	void dev_set_status_all(uint8_t strip, uint8_t status){
		uint32_t color;

		switch(status){
			case LED_STATUS_ERR:
				color = LED_CLR_ERROR;
			break;

			case LED_STATUS_OK:
				color = LED_CLR_OK;
			break;

			case LED_STATUS_DATA:
				color = LED_CLR_DATA;
			break;

			case LED_STATUS_LOAD:
				color = LED_CLR_LOAD;
			break;

			default:
				color = 0;

		}

		//Rozdeli se na dva "pasky" podle nazvu
		for(uint16_t i = 0; i < PERIF_COUNT; i++){
			if(((i & 0x80)>>7) == strip) led_statuses[i] = color;
		}

		//Nastavi se barva
		set_strip_color(strip, ((color>>16) & 0xff), ((color>>8) & 0xff), (color & 0xff));

	}



	//Rutina pro "probliknuti" kdyz jsou na LED nastavena DATA
	void dev_process_pending_status(){
		//Projde vsechny LED
		for(uint16_t i = 0; i < PERIF_COUNT; i++){
			if((i & 0x7f) < LED_BACK_NUMBER){
				if(led_pending_flags[i] != 0){
					//Pokud data nebyla nastavena, nastavi barvu a vycisti flag
					set_color((i >> 7) & 0x01,i & 0x7f, ((led_pending_flags[i]>>16) & 0xff), ((led_pending_flags[i]>>8) & 0xff), (led_pending_flags[i] & 0xff));
					led_pending_flags[i] = 0;
				}else{
					//Nastavi zpet puvodni barvu led
					set_color((i >> 7) & 0x01,i & 0x7f, ((led_statuses[i]>>16) & 0xff), ((led_statuses[i]>>8) & 0xff), (led_statuses[i] & 0xff));
				}
			}
		}

	}

	

	void set_color(uint8_t strip, uint32_t LEDnumber, uint8_t RED, uint8_t GREEN, uint8_t BLUE) {
		

	}

	void set_strip_color(uint8_t strip, uint8_t RED, uint8_t GREEN, uint8_t BLUE){
		uint32_t index;

		for (index = 0; index < ((strip == LED_STRIP_FRONT) ? LED_FRONT_NUMBER : LED_BACK_NUMBER); index++)
			set_color(strip, index, RED, GREEN, BLUE);
	}
	
	void Peripheral::setColor(Color color){
		uint8_t tempBuffer[24];
		uint32_t i;

		for (i = 0; i < 8; i++) // GREEN data
			tempBuffer[i] = ((color.getG() << i) & 0x80) ? LED_1 : LED_0;
		for (i = 0; i < 8; i++) // RED
			tempBuffer[8 + i] = ((color.getR() << i) & 0x80) ? LED_1 : LED_0;
		for (i = 0; i < 8; i++) // BLUE
			tempBuffer[16 + i] = ((color.getB() << i) & 0x80) ? LED_1 : LED_0;

		for (i = 0; i < 24; i++)
			ledBackBuffer[LED_RESET_SLOTS_BEGIN + (int)this->led * 24 + i] = tempBuffer[i];
	}
}
