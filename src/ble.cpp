#include "ble.hpp"
#include "menu.hpp"
#include "comm.hpp"

#include <stdio.h>
#include <string.h>
#include <string>

#include <stm32/gpio.h>
#include <stm32/timer.h>
#include <stm32/dma.h>
#include <stm32/dmamux.h>
#include <stm32/usart.h>
#include <stm32/rcc.h>
#include <cm3/nvic.h>

/*
USART 2 - 
PA2 - TX
PA3 - RX
PA0 - MODE
PA1 - RST


*/
using namespace std;

extern "C" uint32_t usb_cdc_tx(void *buf, int len);

namespace BLE{

	unsigned char bleFifo[256];

	uint8_t bleFifoIndex = 0, bleGotMessage;

	void init(void){

		/*gpio_mode_setup(GPIO::PORTA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO::PIN0 | GPIO::PIN1);
		gpio_set_output_options(GPIO::PORTA, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, GPIO::PIN0);
		gpio_set_output_options(GPIO::PORTA, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, GPIO::PIN1);


		gpio_mode_setup(GPIO::PORTA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO::PIN2);
		gpio_mode_setup(GPIO::PORTA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO::PIN3);
		gpio_set_af(GPIO::PORTA, GPIO_AF7, GPIO::PIN2);
		gpio_set_af(GPIO::PORTA, GPIO_AF7, GPIO::PIN3);



		gpio_set(GPIO::PORTA, GPIO::PIN0);
		gpio_clear(GPIO::PORTA, GPIO::PIN1);
		*/


		//Prijimani DMA
		dma_set_priority(DMA1, DMA_CHANNEL1, DMA_CCR_PL_MEDIUM);
		dma_set_memory_size(DMA1, DMA_CHANNEL1, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA1, DMA_CHANNEL1, DMA_CCR_PSIZE_8BIT);
		dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL1);
		dma_set_read_from_peripheral(DMA1, DMA_CHANNEL1);

		dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL1);
		nvic_enable_irq(NVIC_DMA1_CHANNEL1_IRQ);

		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL1, DMAMUX_CxCR_DMAREQ_ID_UART2_RX);

		dma_set_peripheral_address(DMA1, DMA_CHANNEL1, (uint32_t)&USART2_RDR);
		dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&bleFifo[0]);
		dma_set_number_of_data(DMA1, DMA_CHANNEL1, 1);

		//Vysilani DMA
		dma_set_priority(DMA1, DMA_CHANNEL2, DMA_CCR_PL_MEDIUM);
		dma_set_memory_size(DMA1, DMA_CHANNEL2, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA1, DMA_CHANNEL2, DMA_CCR_PSIZE_8BIT);
		dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL2);
		dma_set_read_from_memory(DMA1, DMA_CHANNEL2);

		dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL2);

		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL2, DMAMUX_CxCR_DMAREQ_ID_UART2_TX);

		usart_set_baudrate(USART2, 115200);
		usart_set_databits(USART2, 8);
		usart_set_parity(USART2, USART_PARITY_NONE);
		usart_set_stopbits(USART2, USART_STOPBITS_1);
		usart_set_flow_control(USART2, USART_FLOWCONTROL_NONE);
		usart_set_mode(USART2, USART_MODE_TX_RX);
		usart_enable_rx_dma(USART2);

		dma_enable_channel(DMA1, DMA_CHANNEL1);
		
		usart_enable(USART2);

		//gpio_set(GPIO::PORTA, GPIO::PIN1);

	}


	void send(std::string data){
		dma_set_peripheral_address(DMA1, DMA_CHANNEL2, (uint32_t)&USART2_TDR);
		dma_set_memory_address(DMA1, DMA_CHANNEL2, (uint32_t)data[0]);
		dma_set_number_of_data(DMA1, DMA_CHANNEL2, data.length());
		usart_enable_tx_dma(USART2);
		nvic_enable_irq(NVIC_DMA1_CHANNEL2_IRQ);
		dma_enable_channel(DMA1, DMA_CHANNEL2);

	}

}

extern "C" void DMA1_Channel2_IRQHandler(){
	dma_disable_channel(DMA1, DMA_CHANNEL2);
	usart_disable_tx_dma(USART2);
	dma_clear_interrupt_flags(DMA1, DMA_CHANNEL2, DMA_TCIF);
    nvic_clear_pending_irq(NVIC_DMA1_CHANNEL2_IRQ);
	
}


extern "C" void DMA1_Channel1_IRQHandler(){
	dma_disable_channel(DMA1, DMA_CHANNEL1);
	usart_disable_rx_dma(USART2);

	dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&BLE::bleFifo[++BLE::bleFifoIndex]);
    dma_set_number_of_data(DMA1, DMA_CHANNEL1, 1);

	if(BLE::bleFifo[BLE::bleFifoIndex-1] == '%'){
		memset(BLE::bleFifo, 0, BLE::bleFifoIndex);
		BLE::bleFifoIndex = 0;
		dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&BLE::bleFifo[BLE::bleFifoIndex]);
	}else if(BLE::bleFifo[BLE::bleFifoIndex-1] == 0x0A){
		comm_decode((char *)BLE::bleFifo, BLE::bleFifoIndex);
		memset(BLE::bleFifo, 0, BLE::bleFifoIndex);
		BLE::bleFifoIndex = 0;
		dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&BLE::bleFifo[BLE::bleFifoIndex]);
	}

	dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_TCIF);
    nvic_clear_pending_irq(NVIC_DMA1_CHANNEL1_IRQ);
	usart_enable_rx_dma(USART2);
	dma_enable_channel(DMA1, DMA_CHANNEL1);
	usart_enable(USART2);
}

