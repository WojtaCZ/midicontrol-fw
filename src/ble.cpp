#include "ble.hpp"
#include "menu.hpp"

#include <stdio.h>
#include <string.h>
#include <string>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/cdc/cdc_device.h>

/*
USART 2 - 
PA2 - TX
PA3 - RX
PA0 - MODE
PA1 - RST
*/
using namespace std;

extern "C" void comm_decode(char * data, int len);

namespace BLE{

	unsigned char bleFifo[256];
	char bleTxBuffer[1024];
	int bleTxBufferPointer = 0;

	uint8_t bleFifoIndex = 0, bleGotMessage;

	bool connected = false;

	void init(void){

		// Enable GPIOA clock
		RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;

		// Configure PA0 and PA1 as output
		GPIOA->MODER &= ~(GPIO_MODER_MODE0_Msk | GPIO_MODER_MODE1_Msk);
		GPIOA->MODER |= (GPIO_MODER_MODE0_0 | GPIO_MODER_MODE1_0);
		GPIOA->OTYPER &= ~(GPIO_OTYPER_OT0 | GPIO_OTYPER_OT1);
		GPIOA->OSPEEDR |= (GPIO_OSPEEDR_OSPEED0_1 | GPIO_OSPEEDR_OSPEED1_1);

		// Configure PA2 and PA3 as alternate function (USART2)
		GPIOA->MODER &= ~(GPIO_MODER_MODE2_Msk | GPIO_MODER_MODE3_Msk);
		GPIOA->MODER |= (GPIO_MODER_MODE2_1 | GPIO_MODER_MODE3_1);
		GPIOA->AFR[0] |= (7 << GPIO_AFRL_AFSEL2_Pos) | (7 << GPIO_AFRL_AFSEL3_Pos);

		// Set PA0 and clear PA1
		GPIOA->BSRR = GPIO_BSRR_BS0;
		GPIOA->BSRR = GPIO_BSRR_BR1;

		// Enable DMA1 clock
		RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;

		// Configure DMA for USART2 RX (DMA1 Channel 1)
		DMA1_Channel1->CCR = DMA_CCR_PL_1 | DMA_CCR_MINC | DMA_CCR_TCIE;
		DMA1_Channel1->CNDTR = 1;
		DMA1_Channel1->CPAR = (uint32_t)&USART2->RDR;
		DMA1_Channel1->CMAR = (uint32_t)bleFifo;

		// Enable DMA1 Channel 1 interrupt
		NVIC_EnableIRQ(DMA1_Channel1_IRQn);

		// Configure DMA for USART2 TX (DMA1 Channel 2)
		DMA1_Channel2->CCR = DMA_CCR_PL_1 | DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_TCIE;
		DMA1_Channel2->CPAR = (uint32_t)&USART2->TDR;

		// Enable DMA1 Channel 2 interrupt
		NVIC_EnableIRQ(DMA1_Channel2_IRQn);

		// Enable USART2 clock
		RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;

		// Configure USART2
		USART2->BRR = 144000000 / 115200;
		USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
		USART2->CR3 = USART_CR3_DMAR | USART_CR3_DMAT;

		// Enable DMA1 Channel 1
		DMA1_Channel1->CCR |= DMA_CCR_EN;

		// Set PA1
		GPIOA->BSRR = GPIO_BSRR_BS1;
	}


	void send(std::string data){
		// Copy the data into the local buffer (clip the size to the maximum 1024)
		memcpy(bleTxBuffer, data.c_str(), std::min((int)data.length(), 1024));

		DMA1_Channel2->CMAR = (uint32_t)bleTxBuffer;
		DMA1_Channel2->CNDTR = data.length();
		DMA1_Channel2->CCR |= DMA_CCR_EN;
	}

	bool isConnected(){
		return connected;
	}

	void clearBuffer(){
		memset(bleFifo, 0, bleFifoIndex);
		bleFifoIndex = 0;
	}

	extern "C" void DMA1_Channel2_IRQHandler(){
		if (DMA1->ISR & DMA_ISR_TCIF2) {
			DMA1->IFCR = DMA_IFCR_CTCIF2;
			DMA1_Channel2->CCR &= ~DMA_CCR_EN;
		}
	}

	extern "C" void DMA1_Channel1_IRQHandler(){
		if (DMA1->ISR & DMA_ISR_TCIF1) {
			DMA1->IFCR = DMA_IFCR_CTCIF1;
			DMA1_Channel1->CCR &= ~DMA_CCR_EN;

			bleFifoIndex++;
			DMA1_Channel1->CMAR = (uint32_t)&bleFifo[bleFifoIndex];
			DMA1_Channel1->CNDTR = 1;
			DMA1_Channel1->CCR |= DMA_CCR_EN;

			if (bleFifo[bleFifoIndex-1] == '%') {
				if (string((char *)&bleFifo[0]).find("STREAM_OPEN") != string::npos) {
					connected = true;
					GUI::renderForce();
				} else if (string((char *)&bleFifo[0]).find("DISCONNECT") != string::npos) {
					connected = false;
					GUI::renderForce();
				}

				memset(bleFifo, 0, bleFifoIndex);
				bleFifoIndex = 0;
			} else if (bleFifo[bleFifoIndex-1] == 0x0A) {
				comm_decode((char *)bleFifo, bleFifoIndex);
				memset(bleFifo, 0, bleFifoIndex);
				bleFifoIndex = 0;
			}
		}
	}
}

// Wrappers to allow sending data from C files
extern "C" void ble_send(char * data, int len){
	std::string str(data, len);
	BLE::send(str);
}

extern "C" bool ble_isConnected(){
	return BLE::isConnected();
}

extern "C" void ble_loadBuffer(char * data, int len){
	memcpy(&BLE::bleTxBuffer[BLE::bleTxBufferPointer], data, len);
	BLE::bleTxBufferPointer += len;

	std::string data_str(data, len);

	if(data_str.find("\r\n") != string::npos){
		ble_send(&BLE::bleTxBuffer[0], BLE::bleTxBufferPointer);
		BLE::bleTxBufferPointer = 0;
	}
}
