#include "global.hpp"
#include "ble.hpp"
#include "usb.h"
#include "menu.hpp"
#include <string.h>

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

unsigned char bleFifo[256];

uint8_t bleFifoIndex = 0, bleGotMessage;

void ble_init(void){

	gpio_mode_setup(PORT_BLE_MODE, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_BLE_MODE | GPIO_BLE_RST);
	gpio_set_output_options(PORT_BLE_MODE, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, GPIO_BLE_MODE);
	gpio_set_output_options(PORT_BLE_RST, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, GPIO_BLE_RST);


    gpio_mode_setup(PORT_USART_BLE_RX, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_USART_BLE_RX);
	gpio_mode_setup(PORT_USART_BLE_TX, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_USART_BLE_TX);
	gpio_set_af(PORT_USART_BLE_RX, GPIO_AF7, GPIO_USART_BLE_RX);
	gpio_set_af(PORT_USART_BLE_TX, GPIO_AF7, GPIO_USART_BLE_TX);



	gpio_set(PORT_BLE_MODE, GPIO_BLE_MODE);
	gpio_clear(PORT_BLE_RST, GPIO_BLE_RST);
	


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

	gpio_set(PORT_BLE_RST, GPIO_BLE_RST);

}


void ble_send(uint8_t * buff, int len){
	dma_set_peripheral_address(DMA1, DMA_CHANNEL2, (uint32_t)&USART2_TDR);
	dma_set_memory_address(DMA1, DMA_CHANNEL2, (uint32_t)buff);
    dma_set_number_of_data(DMA1, DMA_CHANNEL2, len);
	usart_enable_tx_dma(USART2);
	nvic_enable_irq(NVIC_DMA1_CHANNEL2_IRQ);
	dma_enable_channel(DMA1, DMA_CHANNEL2);

}

void dma1_channel2_isr(){
	dma_disable_channel(DMA1, DMA_CHANNEL2);
	usart_disable_tx_dma(USART2);
	dma_clear_interrupt_flags(DMA1, DMA_CHANNEL2, DMA_TCIF);
    nvic_clear_pending_irq(NVIC_DMA1_CHANNEL2_IRQ);
	
}


void dma1_channel1_isr(){
	dma_disable_channel(DMA1, DMA_CHANNEL1);
	usart_disable_rx_dma(USART2);

	dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&bleFifo[++bleFifoIndex]);
    dma_set_number_of_data(DMA1, DMA_CHANNEL1, 1);

	if(bleFifo[bleFifoIndex-1] == '%'){
		memset(bleFifo, 0, bleFifoIndex);
		bleFifoIndex = 0;
		dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&bleFifo[bleFifoIndex]);
	}else if(bleFifo[bleFifoIndex-1] == 0x0A){
		usb_cdc_tx(bleFifo, bleFifoIndex);
		memset(bleFifo, 0, bleFifoIndex);
		bleFifoIndex = 0;
		dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&bleFifo[bleFifoIndex]);
	}

	dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_TCIF);
    nvic_clear_pending_irq(NVIC_DMA1_CHANNEL1_IRQ);
	usart_enable_rx_dma(USART2);
	dma_enable_channel(DMA1, DMA_CHANNEL1);
	usart_enable(USART2);
}

