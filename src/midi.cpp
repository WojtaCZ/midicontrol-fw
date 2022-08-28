#include "midi.hpp"
#include "base.hpp"

#include <stm32/gpio.h>
#include <stm32/timer.h>
#include <stm32/dma.h>
#include <stm32/dmamux.h>
#include <stm32/usart.h>
#include <stm32/rcc.h>
#include <cm3/nvic.h>
#include <string.h>
#include <array>

//Functions to transmit midi over usb
extern "C" uint32_t usb_cdc_tx(void *buf, int len);
extern "C" uint32_t usb_midi_tx(void *buf, int len);

namespace MIDI{
	array<uint8_t, 100> fifo;
	int fifoIndex = 0;
	bool gotMessage = 0;
	vector<byte> fifoData;

	void init(void){

		gpio_mode_setup(GPIO::PORTB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO::PIN10);
		gpio_mode_setup(GPIO::PORTB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO::PIN11);
		gpio_set_af(GPIO::PORTB, GPIO_AF7, GPIO::PIN10);
		gpio_set_af(GPIO::PORTB, GPIO_AF7, GPIO::PIN11);


		//Initialize RX DMA
		dma_set_priority(DMA1, DMA_CHANNEL4, DMA_CCR_PL_VERY_HIGH);
		dma_set_memory_size(DMA1, DMA_CHANNEL4, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA1, DMA_CHANNEL4, DMA_CCR_PSIZE_8BIT);
		dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL4);
		dma_set_read_from_peripheral(DMA1, DMA_CHANNEL4);

		dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL4);
		nvic_enable_irq(NVIC_DMA1_CHANNEL4_IRQ);

		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL4, DMAMUX_CxCR_DMAREQ_ID_UART3_RX);

		dma_set_peripheral_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&USART3_RDR));
		dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t> (&fifo[0]));
		dma_set_number_of_data(DMA1, DMA_CHANNEL4, 1);

		//Initialize TX DMA
		dma_set_priority(DMA1, DMA_CHANNEL5, DMA_CCR_PL_VERY_HIGH);
		dma_set_memory_size(DMA1, DMA_CHANNEL5, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA1, DMA_CHANNEL5, DMA_CCR_PSIZE_8BIT);
		dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL5);
		dma_set_read_from_memory(DMA1, DMA_CHANNEL5);

		dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL5);

		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL5, DMAMUX_CxCR_DMAREQ_ID_UART3_TX);

		usart_set_baudrate(USART3, 31250);
		usart_set_databits(USART3, 8);
		usart_set_parity(USART3, USART_PARITY_NONE);
		usart_set_stopbits(USART3, USART_STOPBITS_1);
		usart_set_flow_control(USART3, USART_FLOWCONTROL_NONE);
		usart_set_mode(USART3, USART_MODE_TX_RX);
		usart_enable_rx_dma(USART3);
		dma_enable_channel(DMA1, DMA_CHANNEL4);
		
		usart_enable(USART3);
	}

	void send(vector<byte> data){
		fifoData = data;
		//Send the payload over MIDI
		dma_set_peripheral_address(DMA1, DMA_CHANNEL5, reinterpret_cast<uint32_t>(&USART3_TDR));
		dma_set_memory_address(DMA1, DMA_CHANNEL5, reinterpret_cast<uint32_t>(&fifoData[0]));
		dma_set_number_of_data(DMA1, DMA_CHANNEL5, data.size());
		usart_enable_tx_dma(USART3);
		nvic_enable_irq(NVIC_DMA1_CHANNEL5_IRQ);
		dma_enable_channel(DMA1, DMA_CHANNEL5);
	}

	//Transmit handler for MIDI
	extern "C" void DMA1_Channel5_IRQHandler(void){
		dma_disable_channel(DMA1, DMA_CHANNEL5);
		usart_disable_tx_dma(USART3);
		dma_clear_interrupt_flags(DMA1, DMA_CHANNEL5, DMA_TCIF);
		nvic_clear_pending_irq(NVIC_DMA1_CHANNEL5_IRQ);
	}


	//Receive handler for midi
	extern "C" void DMA1_Channel4_IRQHandler(void){
		//Disable DMA so it doesnt fire while were in the interrupt
		dma_disable_channel(DMA1, DMA_CHANNEL4);
		usart_disable_rx_dma(USART3);

		//Read the message type
		uint8_t msgType = MIDI::fifo.at(0);

		//If the message type is valid
		if((msgType & 0xF0) >= 0x80 && !MIDI::gotMessage){
			MIDI::gotMessage = true;

			//Treat messages with 2 bytes
			if((msgType >= 0x80 && msgType <= 0xBF) || (msgType & 0xF0) == 0xE0 || msgType == 0xF2 || msgType == 0xF0){
				dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex]));
				dma_set_number_of_data(DMA1, DMA_CHANNEL4, 2);
				MIDI::fifoIndex += 2;
			}else if((msgType & 0xF0) == 0xC0 ||  (msgType & 0xF0) == 0xD0 || msgType == 0xF3 || msgType == 0xF1){
				//Treat messages with one byte
				dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]));
				dma_set_number_of_data(DMA1, DMA_CHANNEL4, 1);
			}else{
				MIDI::fifoIndex = 0;
				dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]));
				dma_set_number_of_data(DMA1, DMA_CHANNEL4, 1);
			}


		}else if(MIDI::gotMessage){
			//If the message type was sysex and we got sysex end
			if(msgType == 0xF0 && MIDI::fifo.at(MIDI::fifoIndex-1) == 0xF7){
				MIDI::fifoIndex = 0;
				MIDI::fifo.at(MIDI::fifoIndex) = 0;
				MIDI::gotMessage = false;
				dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]));
				dma_set_number_of_data(DMA1, DMA_CHANNEL4, 1);
			}else if(msgType == 0xF0 && MIDI::fifo.at(MIDI::fifoIndex-1) != 0xF7){
				//If the message type was sysex and we got data
				dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]));
				dma_set_number_of_data(DMA1, DMA_CHANNEL4, 1);
			}else{
				//Other messages
				//Create a buffer to send over usb
				array<uint8_t, 4> buffer = {(uint8_t)((uint8_t)(MIDI::fifo.at(0) >> 4) & 0x0F), MIDI::fifo.at(0),MIDI::fifo.at(1),MIDI::fifo.at(2)};

				//Transmit the buffer
				usb_midi_tx(&buffer[0], 4);

				//Begin a new receive
				MIDI::fifoIndex = 0;
				MIDI::gotMessage = false;
				MIDI::fifo.at(MIDI::fifoIndex) = 0;
				dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]));
				dma_set_number_of_data(DMA1, DMA_CHANNEL4, 1);
			}
		}else{
			//If an invalid midi message was received, continue receiving
			MIDI::gotMessage = false;
			MIDI::fifoIndex = 0;
			MIDI::fifo.at(MIDI::fifoIndex) = 0;
			dma_set_memory_address(DMA1, DMA_CHANNEL4, reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]));
			dma_set_number_of_data(DMA1, DMA_CHANNEL4, 1);
		}

		//Reenable the DMA
		dma_clear_interrupt_flags(DMA1, DMA_CHANNEL4, DMA_TCIF);
		nvic_clear_pending_irq(NVIC_DMA1_CHANNEL4_IRQ);
		usart_enable_rx_dma(USART3);
		dma_enable_channel(DMA1, DMA_CHANNEL4);
		usart_enable(USART3);
	}

}

//Wrapper for usb.h to call
extern "C" void midi_send(char * data, int len){
	//Create vector from provided data
	vector<byte> payload;
	for(int i = 0; i < len; i++) payload.push_back((byte)data[i]);
	//Call the send function
	MIDI::send(payload);
}
