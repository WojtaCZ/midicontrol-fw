#include "bluetooth.hpp"
#include "menu.hpp"

#include <stdio.h>
#include <string.h>
#include <string>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cstdint>
#include <cmsis_compiler.h>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/midi/midi_device.h>

#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"


/*
USART 2 - 
PA2 - TX
PA3 - RX
PA0 - MODE
PA1 - RST
*/
using namespace std;

extern "C" void comm_decode(char * data, int len);

namespace Bluetooth{

	static constexpr int DEFAUILT_TIMEOUT = 1000;
	static constexpr int BUFF_SIZE = 256;

	Mode _mode = Mode::DATA;
	StatusMessage _lastStatus = StatusMessage::REBOOT;

	char rxBuffer[BUFF_SIZE], txBuffer[BUFF_SIZE];
	uint8_t rxIndex = 0;


	uint8_t setAuthentication(Authentication auth) {
		//return static_cast<uint8_t>(sendCommand("SA," + std::to_string(static_cast<uint8_t>(auth))));
		return 0;
	}
	
	CommandResponse sendCommand(string command, bool awaitResponse = true, int timeout = DEFAUILT_TIMEOUT) {
		string response;
		return CommandResponse::OK;
		//return sendCommand(command, response, awaitResponse, timeout);
	}

	Mode setMode(Mode mode) {
		if(mode == mode) return mode;

		if (mode == Mode::COMMAND) {
			_send_string("$$$");
			while(_mode != Mode::COMMAND);
		} else {
			_send_string("---\r");
			while(_mode != Mode::DATA);
		}
		return mode;
	}


	CommandResponse sendCommand(string command, string & response, bool awaitResponse = true, int timeout = DEFAUILT_TIMEOUT) {
		if (_mode != Mode::COMMAND) {
			_mode = setMode(Mode::COMMAND);
		}

		_send_string(command + "\r");

		if (awaitResponse) {
			// Wait for response (this is a placeholder, actual implementation may vary)
			// In a real implementation, you would wait and parse the response from the BLE module
			// Here we just assume success for demonstration purposes
			return CommandResponse::OK;
		}
		return CommandResponse::OK;

	}



	inline void _receive_handler() {
		if(!strcmp(rxBuffer, "CMD>")){
			_mode = Mode::COMMAND;
			_clear_rx_buffer();
		}

		if(!strcmp(rxBuffer, "END")){
			_mode = Mode::DATA;
			_clear_rx_buffer();
		}

		if(rxBuffer[0] == '%' && rxIndex != 0 && rxBuffer[rxIndex] == '%'){
			if      (strcmp(rxBuffer, "%ADV_TIMEOUT%") == 0)      _lastStatus = StatusMessage::ADV_TIMEOUT;
			else if (strcmp(rxBuffer, "%BONDED%") == 0)           _lastStatus = StatusMessage::BONDED;
			else if (strcmp(rxBuffer, "%DISCONNECT%") == 0)       _lastStatus = StatusMessage::DISCONNECT;
			else if (strcmp(rxBuffer, "%ERR_CONNPARAM%") == 0)    _lastStatus = StatusMessage::ERR_CONNPARAM;
			else if (strcmp(rxBuffer, "%ERR_MEMORY%") == 0)       _lastStatus = StatusMessage::ERR_MEMORY;
			else if (strcmp(rxBuffer, "%ERR_READ%") == 0)         _lastStatus = StatusMessage::ERR_READ;
			else if (strcmp(rxBuffer, "%ERR_RMT_CMD%") == 0)      _lastStatus = StatusMessage::ERR_RMT_CMD;
			else if (strcmp(rxBuffer, "%ERR_SEC%") == 0)          _lastStatus = StatusMessage::ERR_SEC;
			else if (strcmp(rxBuffer, "%KEY_REQ%") == 0)          _lastStatus = StatusMessage::KEY_REQ;
			else if (strcmp(rxBuffer, "%PIO1H%") == 0)            _lastStatus = StatusMessage::PIO1H;
			else if (strcmp(rxBuffer, "%PIO1L%") == 0)            _lastStatus = StatusMessage::PIO1L;
			else if (strcmp(rxBuffer, "%PIO2H%") == 0)            _lastStatus = StatusMessage::PIO2H;
			else if (strcmp(rxBuffer, "%PIO2L%") == 0)            _lastStatus = StatusMessage::PIO2L;
			else if (strcmp(rxBuffer, "%PIO3H%") == 0)            _lastStatus = StatusMessage::PIO3H;
			else if (strcmp(rxBuffer, "%PIO3L%") == 0)            _lastStatus = StatusMessage::PIO3L;
			else if (strcmp(rxBuffer, "%RE_DISCV%") == 0)         _lastStatus = StatusMessage::RE_DISCV;
			else if (strcmp(rxBuffer, "%REBOOT%") == 0)           _lastStatus = StatusMessage::REBOOT;
			else if (strcmp(rxBuffer, "%RMT_CMD_OFF%") == 0)      _lastStatus = StatusMessage::RMT_CMD_OFF;
			else if (strcmp(rxBuffer, "%RMT_CMD_ON%") == 0)       _lastStatus = StatusMessage::RMT_CMD_ON;
			else if (strcmp(rxBuffer, "%SECURED%") == 0)          _lastStatus = StatusMessage::SECURED;
			else if (strcmp(rxBuffer, "%STREAM_OPEN%") == 0)      _lastStatus = StatusMessage::STREAM_OPEN;
			else if (strcmp(rxBuffer, "%TMR1%") == 0)             _lastStatus = StatusMessage::TMR1;
			else if (strcmp(rxBuffer, "%TMR2%") == 0)             _lastStatus = StatusMessage::TMR2;
			else if (strcmp(rxBuffer, "%TMR3%") == 0)             _lastStatus = StatusMessage::TMR3;

			// --- Parameterized messages ---
			else if (strncmp(rxBuffer, "%CONN_PARAM,", 12) == 0) {
				_lastStatus = StatusMessage::CONN_PARAM;
				int interval, latency, timeout;
				sscanf(rxBuffer, "%%CONN_PARAM,%d,%d,%d%%", &interval, &latency, &timeout);
			}
			else if (strncmp(rxBuffer, "%CONNECT", 8) == 0) {
				_lastStatus = StatusMessage::CONNECT;
				//sscanf(rxBuffer, "%%CONNECT,%d,%31[^%%]%%", &msg.connected, msg.addr);
			}
			else if (strncmp(rxBuffer, "%KEY:", 5) == 0) {
				_lastStatus = StatusMessage::KEY;
				int key;
				sscanf(rxBuffer, "%%KEY:%d%%", &key);
			}
			else if (strncmp(rxBuffer, "%INDI,", 6) == 0) {
				_lastStatus = StatusMessage::INDI;
				//sscanf(rxBuffer, "%%INDI,%d,%127[^%%]%%", &msg.hdl, msg.hex);
			}
			else if (strncmp(rxBuffer, "%NOTI,", 6) == 0) {
				_lastStatus = StatusMessage::NOTI;
				//sscanf(rxBuffer, "%%NOTI,%d,%127[^%%]%%", &msg.hdl, msg.hex);
			}
			else if (strncmp(rxBuffer, "%RV,", 4) == 0) {
				_lastStatus = StatusMessage::RV;
				//sscanf(rxBuffer, "%%RV,%d,%127[^%%]%%", &msg.hdl, msg.hex);
			}
			else if (strncmp(rxBuffer, "%S_RUN:", 7) == 0) {
				_lastStatus = StatusMessage::S_RUN;
				//sscanf(rxBuffer, "%%S_RUN:%63[^%%]%%", msg.cmd);
			}
			else if (strncmp(rxBuffer, "%WC,", 4) == 0) {
				_lastStatus = StatusMessage::WC;
				//sscanf(rxBuffer, "%%WC,%d,%127[^%%]%%", &msg.hdl, msg.hex);
			}
			else if (strncmp(rxBuffer, "%WV,", 4) == 0) {
				_lastStatus = StatusMessage::WV;
				//sscanf(rxBuffer, "%%WV,%d,%127[^%%]%%", &msg.hdl, msg.hex);
			}

			// --- Advertisement messages ---
			else if (rxBuffer[0] == '%' && strstr(rxBuffer, ",UUIDs,") != NULL) {
				_lastStatus = StatusMessage::ADV_CONNECTABLE;
				char addr[32], name[32], uuids[64];
				int connected, rssi;
				sscanf(rxBuffer, "%%%31[^,],%d,%31[^,],%63[^,],%d%%", addr, &connected, name, uuids, &rssi);
			}
			else if (rxBuffer[0] == '%' && strstr(rxBuffer, ",Brcst,") != NULL) {
				_lastStatus = StatusMessage::ADV_NON_CONNECTABLE;
				char addr[32], hex[127];
				int connected, rssi;
				sscanf(rxBuffer, "%%%31[^,],%d,%d,Brcst,%127[^%%]%%", addr, &connected, &rssi, hex);
			}
			_clear_rx_buffer();
		}



	}

	inline void _clear_rx_buffer() {
		memset(rxBuffer, 0, BUFF_SIZE);
 		stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(&txBuffer[0]));
        stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), 1);
		rxIndex = 0;
	}

	uint8_t _send_string(std::string data) {
		if(data.length() > BUFF_SIZE) return 1;
		if(DMA1_Channel2->CCR & DMA_CCR_EN) return 1;

		memcpy(txBuffer, data.c_str(), data.length());

		stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(&txBuffer[0]));
		stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), data.length());
		stmcpp::reg::set(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);

		return 0;
	}

	uint8_t _send_char(char c) {
		if(DMA1_Channel2->CCR & DMA_CCR_EN) return 1;

		txBuffer[0] = c;
		stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(&txBuffer[0]));
		stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), 1);
		stmcpp::reg::set(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);

		return 0;
	}

	uint8_t init(){
		// USART GPIOs
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 2> usart_tx (stmcpp::gpio::mode::af7, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 3> usart_rx (stmcpp::gpio::mode::af7);

		// RN 487x control pins
        stmcpp::gpio::pin<stmcpp::gpio::port::porta, 0> mode (stmcpp::gpio::mode::output, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 1> rst_n (stmcpp::gpio::mode::output, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		
		// Go into application mode
		mode.set();

		// Reset the RN487x
		rst_n.clear();

		// Set up RX dma
        stmcpp::reg::write(std::ref(DMA1_Channel1->CCR), 
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
            (0b1 << DMA_CCR_TCIE_Pos)   // Transfer complete interrupt enable
            
        );

        // Set up the DMA memory and peripheral
        stmcpp::reg::write(std::ref(DMA1_Channel1->CMAR), reinterpret_cast<uint32_t>(&rxBuffer[0]));
        stmcpp::reg::write(std::ref(DMA1_Channel1->CPAR), reinterpret_cast<uint32_t>(&USART2->RDR));
        stmcpp::reg::write(std::ref(DMA1_Channel1->CNDTR), 1);
        NVIC_EnableIRQ(DMA1_Channel1_IRQn);

        // Set up DMAMUX routing (DMAMUX channels map to 6-11 to DMA2 1-6, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel0->CCR), (26 << DMAMUX_CxCR_DMAREQ_ID_Pos));

		// Set up TX dma
        stmcpp::reg::write(std::ref(DMA1_Channel2->CCR), 
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
			(0b1 << DMA_CCR_DIR_Pos)   | // Read from memory
            (0b1 << DMA_CCR_TCIE_Pos)   // Transfer complete interrupt enable
            
        );

        // Set up the DMA memory and peripheral
        stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(&txBuffer[0]));
        stmcpp::reg::write(std::ref(DMA1_Channel2->CPAR), reinterpret_cast<uint32_t>(&USART2->TDR));
        stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), 1);
        NVIC_EnableIRQ(DMA1_Channel2_IRQn);

        // Set up DMAMUX routing (DMAMUX channels map to 6-11 to DMA2 1-6, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel1->CCR), (27 << DMAMUX_CxCR_DMAREQ_ID_Pos));

		// Set up USART2
		stmcpp::reg::write(std::ref(USART2->BRR), 144000000 / 115200); // Assuming 144MHz clock, set baud rate to 115200
		stmcpp::reg::write(std::ref(USART2->CR1), 
			(0b1 << USART_CR1_TE_Pos) | // Transmitter enable
			(0b1 << USART_CR1_RE_Pos)   // Receiver enable
		);

		stmcpp::reg::write(std::ref(USART2->CR3), 
			(0b1 << USART_CR3_DMAR_Pos) | // DMA receiver enable
			(0b1 << USART_CR3_DMAT_Pos)    // DMA transmitter enable
		);

		// Enable USART2
		stmcpp::reg::set(std::ref(USART2->CR1), USART_CR1_UE);

		// Enable RX DMA
		stmcpp::reg::set(std::ref(DMA1_Channel1->CCR), DMA_CCR_EN);

		// Bring RN487x out of reset
		rst_n.set();


       
		return 0;
	}



	extern "C" void DMA1_Channel2_IRQHandler(){
		// Disable TX DMA
		if(stmcpp::reg::read(std::ref(DMA1->ISR)) & DMA_ISR_TCIF2){
			stmcpp::reg::clear(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);
            stmcpp::reg::set(std::ref(DMA1->IFCR), DMA_IFCR_CTCIF2); 
        }
	}

	extern "C" void DMA1_Channel1_IRQHandler(){
		if(stmcpp::reg::read(std::ref(DMA1->ISR)) & DMA_ISR_TCIF1){
			stmcpp::reg::clear(std::ref(DMA1_Channel1->CCR), DMA_CCR_EN);
			_receive_handler();
			rxIndex++;
			stmcpp::reg::set(std::ref(DMA1_Channel1->CCR), DMA_CCR_EN);
            stmcpp::reg::set(std::ref(DMA1->IFCR), DMA_IFCR_CTCIF1); 
        }
	}
}

