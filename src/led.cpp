#include "led.hpp"
#include "base.hpp"

#include <stm32/timer.h>
#include <stm32/gpio.h>
#include <stm32/dma.h>
#include <stm32/dmamux.h>
#include <cm3/nvic.h>

namespace LED{

	Peripheral USB(LedID::USB);
	Peripheral Display(LedID::DISPLAY);
	Peripheral Current(LedID::CURRENT);
	Peripheral MIDIA(LedID::MIDIA);
	Peripheral MIDIB(LedID::MIDIB);
	Peripheral Bluetooth(LedID::BLUETOOTH);

	uint8_t ledFrontBuffer[LED_FRONT_BUFFER_SIZE];
	uint8_t ledBackBuffer[LED_BACK_BUFFER_SIZE];

	uint32_t led_pending_flags[LED_BACK_NUMBER];
	uint32_t led_statuses[LED_BACK_NUMBER];

	void init(){

		gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, Gpio::GPIO_FRONT_LED | Gpio::GPIO_BACK_LED);
		gpio_set_output_options(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_100MHZ, Gpio::GPIO_FRONT_LED | Gpio::GPIO_BACK_LED);
		gpio_set_af(GPIOB, GPIO_AF10, Gpio::GPIO_BACK_LED);
		gpio_set_af(GPIOB, GPIO_AF1, Gpio::GPIO_FRONT_LED);

		//Setup for back led
		dma_set_priority(DMA2, DMA_CHANNEL1, DMA_CCR_PL_LOW);
		dma_set_memory_size(DMA2, DMA_CHANNEL1, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA2, DMA_CHANNEL1, DMA_CCR_PSIZE_16BIT);
		dma_enable_memory_increment_mode(DMA2, DMA_CHANNEL1);
		dma_enable_circular_mode(DMA2, DMA_CHANNEL1);
		dma_set_read_from_memory(DMA2, DMA_CHANNEL1);

		//Plus 8 offset to select channels intended for DMA 2 - libopencm3 doesnt have a define for this :(
		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL1+8, DMAMUX_CxCR_DMAREQ_ID_TIM17_CH1);

		dma_set_peripheral_address(DMA2, DMA_CHANNEL1, (uint32_t)&TIM17_CCR1);
		dma_set_memory_address(DMA2, DMA_CHANNEL1, (uint32_t)&ledBackBuffer);
		dma_set_number_of_data(DMA2, DMA_CHANNEL1, LED_BACK_BUFFER_SIZE);

		timer_enable_preload(TIM17);
		timer_update_on_overflow(TIM17);
		timer_set_dma_on_update_event(TIM17);
		timer_enable_irq(TIM17, TIM_DIER_CC1DE);
		timer_generate_event(TIM17, TIM_EGR_CC1G);
		timer_set_oc_mode(TIM17, TIM_OC1, TIM_OCM_PWM1);
		timer_enable_oc_output(TIM17, TIM_OC1);
		timer_enable_break_main_output(TIM17);
		timer_set_period(TIM17, LED_TIMER_PERIOD-1);

		timer_enable_counter(TIM17);
		dma_enable_channel(DMA2, DMA_CHANNEL1);

		//Setup for front led
		dma_set_priority(DMA2, DMA_CHANNEL2, DMA_CCR_PL_LOW);
		dma_set_memory_size(DMA2, DMA_CHANNEL2, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA2, DMA_CHANNEL2, DMA_CCR_PSIZE_16BIT);
		dma_enable_memory_increment_mode(DMA2, DMA_CHANNEL2);
		dma_enable_circular_mode(DMA2, DMA_CHANNEL2);
		dma_set_read_from_memory(DMA2, DMA_CHANNEL2);

		//Plus 8 offset to select channels intended for DMA 2 - libopencm3 doesnt have a define for this :( 
		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL2+8, DMAMUX_CxCR_DMAREQ_ID_TIM15_CH1);

		dma_set_peripheral_address(DMA2, DMA_CHANNEL2, (uint32_t)&TIM15_CCR1);
		dma_set_memory_address(DMA2, DMA_CHANNEL2, (uint32_t)&ledFrontBuffer);
		dma_set_number_of_data(DMA2, DMA_CHANNEL2, LED_FRONT_BUFFER_SIZE);

		timer_enable_preload(TIM15);
		timer_update_on_overflow(TIM15);
		timer_set_dma_on_update_event(TIM15);
		timer_enable_irq(TIM15, TIM_DIER_CC1DE);
		timer_generate_event(TIM15, TIM_EGR_CC1G);
		timer_set_oc_mode(TIM15, TIM_OC1, TIM_OCM_PWM1);
		timer_enable_oc_output(TIM15, TIM_OC1);
		timer_enable_break_main_output(TIM15);
		timer_set_period(TIM15, LED_TIMER_PERIOD-1);

		timer_enable_counter(TIM15);
		dma_enable_channel(DMA2, DMA_CHANNEL2);



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
/*
	void fill_buff_black(uint8_t strip){
		uint32_t index, buffIndex;
		buffIndex = 0;

		for (index = 0; index < LED_RESET_SLOTS_BEGIN; index++) {
			if(strip == LED_STRIP_FRONT){
				ledFrontBuffer[buffIndex] = LED_RESET;
			}else ledBackBuffer[buffIndex] = LED_RESET;
			
			buffIndex++;
		}
		for (index = 0; index < ((strip == LED_STRIP_FRONT) ? LED_FRONT_DATA_SIZE : LED_BACK_DATA_SIZE); index++) {
			if(strip == LED_STRIP_FRONT){
				ledFrontBuffer[buffIndex] = LED_0;
			}else ledBackBuffer[buffIndex] = LED_0;
			buffIndex++;
		}
		buffIndex++;
		for (index = 0; index < LED_RESET_SLOTS_END; index++) {
			if(strip == LED_STRIP_FRONT){
				ledFrontBuffer[buffIndex] = 0;
			}else ledBackBuffer[buffIndex] = 0;
			buffIndex++;
		}
	}

	void fill_buff_white(uint8_t strip){
		uint32_t index, buffIndex;
		buffIndex = 0;

		for (index = 0; index < LED_RESET_SLOTS_BEGIN; index++) {
			if(strip == LED_STRIP_FRONT){
				ledFrontBuffer[buffIndex] = LED_RESET;
			}else ledBackBuffer[buffIndex] = LED_RESET;
			
			buffIndex++;
		}
		for (index = 0; index < ((strip == LED_STRIP_FRONT) ? LED_FRONT_DATA_SIZE : LED_BACK_DATA_SIZE); index++) {
			if(strip == LED_STRIP_FRONT){
				ledFrontBuffer[buffIndex] = LED_1;
			}else ledBackBuffer[buffIndex] = LED_1;
			buffIndex++;
		}
		buffIndex++;
		for (index = 0; index < LED_RESET_SLOTS_END; index++) {
			if(strip == LED_STRIP_FRONT){
				ledFrontBuffer[buffIndex] = 0;
			}else ledBackBuffer[buffIndex] = 0;
			buffIndex++;
		}
	}
*/

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