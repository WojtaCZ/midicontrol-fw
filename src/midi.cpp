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

#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"

using namespace std;

namespace midi {

	uint8_t packet[4];
	static bool receptionOngoing = false;
	static uint8_t receivedIdx = 0;

	void init(void) {

		stmcpp::gpio::pin<stmcpp::gpio::port::portb, 10> usart_tx (stmcpp::gpio::mode::af7, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::portb, 11> usart_rx (stmcpp::gpio::mode::af7);

		// Set up USART3
		stmcpp::reg::write(std::ref(USART3->BRR), 144000000 / 31250); 
		stmcpp::reg::write(std::ref(USART3->CR1), 
			USART_CR1_TE 		| // Transmitter enable
			USART_CR1_RE 		| // Receiver enable
			USART_CR1_RXNEIE 	  // RXNE interrupt enable
		);

		stmcpp::reg::write(std::ref(USART3->CR3), 
			USART_CR3_DMAR  // DMA enable for reception
		);
	
		// Enable USART3 IRQ
		NVIC_EnableIRQ(USART3_IRQn);

		// Enable USART3
		stmcpp::reg::set(std::ref(USART3->CR1), USART_CR1_UE); 

	}

	uint8_t messageSize(uint8_t messageType) {
		switch(messageType){
			case MIDI_CIN_SYSEX_END_1BYTE:
			case MIDI_CIN_1BYTE_DATA:
				return 1;

			case MIDI_CIN_SYSEX_END_2BYTE:
			case MIDI_CIN_PROGRAM_CHANGE:
			case MIDI_CIN_CHANNEL_PRESSURE:
			case MIDI_CIN_SYSCOM_2BYTE:
				return 2;

			case MIDI_CIN_SYSEX_START:
			case MIDI_CIN_SYSEX_END_3BYTE:
			case MIDI_CIN_NOTE_ON:
			case MIDI_CIN_NOTE_OFF:
			case MIDI_CIN_POLY_KEYPRESS:
			case MIDI_CIN_PITCH_BEND_CHANGE:	
			case MIDI_CIN_CONTROL_CHANGE:
			case MIDI_CIN_SYSCOM_3BYTE:
				return 3;

			case MIDI_CIN_MISC:
			case MIDI_CIN_CABLE_EVENT:
			default:
				return 0;
		}
	}

	midi_code_index_number_t statusToCIN (uint8_t status) {
		// Channel Voice Messages
		if ((status & 0xF0) == 0x80) return MIDI_CIN_NOTE_OFF;
		if ((status & 0xF0) == 0x90) return MIDI_CIN_NOTE_ON;
		if ((status & 0xF0) == 0xA0) return MIDI_CIN_POLY_KEYPRESS;
		if ((status & 0xF0) == 0xB0) return MIDI_CIN_CONTROL_CHANGE;
		if ((status & 0xF0) == 0xC0) return MIDI_CIN_PROGRAM_CHANGE;
		if ((status & 0xF0) == 0xD0) return MIDI_CIN_CHANNEL_PRESSURE;
		if ((status & 0xF0) == 0xE0) return MIDI_CIN_PITCH_BEND_CHANGE;

		// System Common Messages
		switch (status) {
			case 0xF0: return MIDI_CIN_SYSEX_START;      // SysEx Start
			case 0xF1: return MIDI_CIN_SYSCOM_2BYTE;    // MTC Quarter Frame
			case 0xF2: return MIDI_CIN_SYSCOM_3BYTE;    // Song Position Pointer
			case 0xF3: return MIDI_CIN_SYSCOM_2BYTE;    // Song Select
			case 0xF6: return MIDI_CIN_1BYTE_DATA;      // Tune Request
			case 0xF7: return MIDI_CIN_SYSEX_END_1BYTE; // SysEx End (if no data)
			case 0xF8:                        // Timing Clock
			case 0xFA:                        // Start
			case 0xFB:                        // Continue
			case 0xFC:                        // Stop
			case 0xFE:                        // Active Sensing
			case 0xFF: return MIDI_CIN_1BYTE_DATA;     // System Reset / undefined 1-byte
			default: return MIDI_CIN_MISC;              // Other undefined bytes
		}
	}

	void send(uint8_t (&packet)[4]) {
		uint8_t size = messageSize(packet[0]);
		for (uint8_t i = 1; i <= size; ++i) {
			// Wait until transmit data register is empty
			while (!(USART3->ISR & USART_ISR_TXE)) {}
			stmcpp::reg::write(std::ref(USART3->TDR), packet[i]);
		}

		// Wait for transmission complete
		while (!(USART3->ISR & USART_ISR_TC)) {}
	}

	bool isStatusByte(uint8_t byte) {
		// Check if the byte is a valid MIDI status byte
		return byte >= 0x80;
	}

	bool isSysEx(uint8_t byte) {
		// Check if the byte is a SysEx start or end byte
		return (byte == 0xF0 || byte == 0xF7);
	}

	// Receive handler for midi
	extern "C" void USART3_IRQHandler(void) {
		if (USART3->ISR & USART_ISR_RXNE) {
			// Read the received byte (this also clears the RXNE flag)
			uint8_t data = stmcpp::reg::read(std::ref(USART3->RDR));

			// If we get channel voice message
			if (isStatusByte(data) && !isSysEx(data)) {
				uint8_t cin = statusToCIN(data);
				uint8_t size = messageSize(cin);

				packet[0] = cin;
				packet[1] = data; // Store the first byte (status byte)

				receptionOngoing = true;
				receivedIdx = 2; 

			}else if (data == 0xF0) { // If we get a Sysex start byte
				uint8_t cin = statusToCIN(data);
				packet[0] = cin;
				packet[1] = data;
				receptionOngoing = true;
				receivedIdx = 2; // Reset the index for received bytes

			}else if (data == 0xF7 && receptionOngoing) { // If we get a Sysex end byte
				packet[receivedIdx++] = data; // Store the end byte
				switch (receivedIdx){
					case 2:
						packet[0] = MIDI_CIN_SYSEX_END_1BYTE; // 1 byte SysEx end
						break;
					case 3:
						packet[0] = MIDI_CIN_SYSEX_END_2BYTE; // 2 byte SysEx end
						break;
					case 4:
					default:
						packet[0] = MIDI_CIN_SYSEX_END_3BYTE; // 3 byte SysEx end
						break;
				}

				tud_midi_packet_write(packet);
				receptionOngoing = false; // End reception
				receivedIdx = 0;

			}else if (receptionOngoing) {
				packet[receivedIdx++] = data; // Store the received byte in the packet
			}

			if(receptionOngoing && receivedIdx > messageSize(packet[0])) {
				// If we have received enough bytes, send the packet
				tud_midi_packet_write(packet);

				if(packet[0] != MIDI_CIN_SYSEX_START) {
					receptionOngoing = false;
					receivedIdx = 0;
				} else {
					receivedIdx = 1;
				}
			}
			
		}

		if (USART3->ISR & USART_ISR_ORE) {
			// Overrun error occurred, clear the ORE flag
			USART3->ICR |= USART_ICR_ORECF;
			// Handle the error (e.g., log it, reset reception state, etc.)
			receptionOngoing = false;
			receivedIdx = 0;
		}
	}
}

