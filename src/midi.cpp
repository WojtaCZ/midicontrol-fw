#include "midi.hpp"
#include "base.hpp"

#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <stm32g431xx.h>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/midi/midi_device.h>

#include <string.h>
#include <array>
#include <vector>

using namespace std;



namespace MIDI {
	array<uint8_t, 100> fifo;
	int fifoIndex = 0;
	bool gotMessage = 0;
	vector<uint8_t> fifoData;

	void init(void) {
		// Enable GPIOB clock
		RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;

		// Set PB10 and PB11 to alternate function mode
		GPIOB->MODER &= ~(GPIO_MODER_MODER10 | GPIO_MODER_MODER11);
		GPIOB->MODER |= (GPIO_MODER_MODER10_1 | GPIO_MODER_MODER11_1);

		// Set alternate function to USART3 (AF7)
		GPIOB->AFR[1] &= ~((0xF << (4 * (10 - 8))) | (0xF << (4 * (11 - 8))));
		GPIOB->AFR[1] |= ((7 << (4 * (10 - 8))) | (7 << (4 * (11 - 8))));

		// Enable DMA1 clock
		RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;

		// Enable USART3 clock
		RCC->APB1ENR1 |= RCC_APB1ENR1_USART3EN;

		// Initialize RX DMA (Channel 4)
		DMA1_Channel4->CCR = DMA_CCR_PL_1 | DMA_CCR_MINC | DMA_CCR_TCIE | DMA_CCR_DIR;
		DMA1_Channel4->CNDTR = 1;
		DMA1_Channel4->CPAR = reinterpret_cast<uint32_t>(&USART3->RDR);
		DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&fifo[0]);
		DMA1_Channel4->CCR |= DMA_CCR_EN;

		// Enable DMA1 Channel4 interrupt
		NVIC_EnableIRQ(DMA1_Channel4_IRQn);

		// Initialize TX DMA (Channel 5)
		DMA1_Channel5->CCR = DMA_CCR_PL_1 | DMA_CCR_MINC | DMA_CCR_TCIE;
		DMA1_Channel5->CNDTR = 1;
		DMA1_Channel5->CPAR = reinterpret_cast<uint32_t>(&USART3->TDR);

		// Enable DMA1 Channel5 interrupt
		NVIC_EnableIRQ(DMA1_Channel5_IRQn);

		// Configure USART3
		USART3->BRR = 42000000 / 31250; // Assuming APB1 clock is 42 MHz
		USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;

		// Enable USART3 DMA requests
		USART3->CR3 |= USART_CR3_DMAT | USART_CR3_DMAR;

		// Enable USART3
		USART3->CR1 |= USART_CR1_UE;
	}

	void send(vector<uint8_t> data) {
		fifoData = data;
		// Send the payload over MIDI
		DMA1_Channel5->CMAR = reinterpret_cast<uint32_t>(&fifoData[0]);
		DMA1_Channel5->CNDTR = data.size();
		DMA1_Channel5->CCR |= DMA_CCR_EN;
	}

	// Transmit handler for MIDI
	extern "C" void DMA1_Channel5_IRQHandler(void) {
		if (DMA1->ISR & DMA_ISR_TCIF5) {
			DMA1->IFCR |= DMA_IFCR_CTCIF5;
			DMA1_Channel5->CCR &= ~DMA_CCR_EN;
		}
	}

	// Receive handler for MIDI
	extern "C" void DMA1_Channel4_IRQHandler(void) {
		if (DMA1->ISR & DMA_ISR_TCIF4) {
			DMA1->IFCR |= DMA_IFCR_CTCIF4;
			DMA1_Channel4->CCR &= ~DMA_CCR_EN;

			// Read the message type
			uint8_t msgType = MIDI::fifo.at(0);

			// If the message type is valid
			if ((msgType & 0xF0) >= 0x80 && !MIDI::gotMessage) {
				MIDI::gotMessage = true;

				// Treat messages with 2 bytes
				if ((msgType >= 0x80 && msgType <= 0xBF) || (msgType & 0xF0) == 0xE0 || msgType == 0xF2 || msgType == 0xF0) {
					DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex]);
					DMA1_Channel4->CNDTR = 2;
					MIDI::fifoIndex += 2;
				} else if ((msgType & 0xF0) == 0xC0 || (msgType & 0xF0) == 0xD0 || msgType == 0xF3 || msgType == 0xF1) {
					// Treat messages with one byte
					DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]);
					DMA1_Channel4->CNDTR = 1;
				} else {
					MIDI::fifoIndex = 0;
					DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]);
					DMA1_Channel4->CNDTR = 1;
				}
			} else if (MIDI::gotMessage) {
				// If the message type was sysex and we got sysex end
				if (msgType == 0xF0 && MIDI::fifo.at(MIDI::fifoIndex - 1) == 0xF7) {
					MIDI::fifoIndex = 0;
					MIDI::fifo.at(MIDI::fifoIndex) = 0;
					MIDI::gotMessage = false;
					DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]);
					DMA1_Channel4->CNDTR = 1;
				} else if (msgType == 0xF0 && MIDI::fifo.at(MIDI::fifoIndex - 1) != 0xF7) {
					// If the message type was sysex and we got data
					DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]);
					DMA1_Channel4->CNDTR = 1;
				} else {
					// Other messages
					// Create a buffer to send over usb
					array<uint8_t, 4> buffer = {(uint8_t)((uint8_t)(MIDI::fifo.at(0) >> 4) & 0x0F), MIDI::fifo.at(0), MIDI::fifo.at(1), MIDI::fifo.at(2)};

					tud_midi_packet_write(&buffer[0]);

					// Begin a new receive
					MIDI::fifoIndex = 0;
					MIDI::gotMessage = false;
					MIDI::fifo.at(MIDI::fifoIndex) = 0;
					DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]);
					DMA1_Channel4->CNDTR = 1;
				}
			} else {
				// If an invalid midi message was received, continue receiving
				MIDI::gotMessage = false;
				MIDI::fifoIndex = 0;
				MIDI::fifo.at(MIDI::fifoIndex) = 0;
				DMA1_Channel4->CMAR = reinterpret_cast<uint32_t>(&MIDI::fifo[MIDI::fifoIndex++]);
				DMA1_Channel4->CNDTR = 1;
			}

			// Re-enable the DMA
			DMA1_Channel4->CCR |= DMA_CCR_EN;
		}
	}
}

// Wrapper for usb.h to call
extern "C" void midi_send(char *data, int len) {
	// Create vector from provided data
	vector<uint8_t> payload;
	for (int i = 0; i < len; i++) payload.push_back((uint8_t)data[i]);
	// Call the send function
	MIDI::send(payload);
}
