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
#include "stmcpp/units.hpp"
#include "stmcpp/systick.hpp"


/*
USART 2 - 
PA2 - TX
PA3 - RX
PA0 - MODE
PA1 - RST
*/
using namespace std;
using namespace stmcpp::units;

extern "C" void comm_decode(char * data, int len);

namespace Bluetooth{

	static constexpr int DEFAUILT_TIMEOUT = 1000;
	static constexpr int BUFF_SIZE = 256;

	Mode _mode = Mode::DATA;

	bool _receivingCommand = false;
	char _rxCommandBuffer[BUFF_SIZE], _rxDataBuffer[BUFF_SIZE], _txBuffer[BUFF_SIZE];
	uint8_t _rxCommandIndex = 0, _rxDataIndex = 0;

	std::deque<std::string> _commandTextQueue;


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
		if(mode == _mode) return mode;

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

	std::unique_ptr<Command> _build_command_from_string(const std::string& cmdText) {
		// Use .c_str() to allow strcmp, strncmp, and sscanf to work
		const char* rxCmd = cmdText.c_str(); 

		// --- Simple (non-parameterized) commands ---
		if      (!strcmp(rxCmd, "ADV_TIMEOUT"))      return make_unique<Command>(Command::Type::ADV_TIMEOUT);
		else if (!strcmp(rxCmd, "BONDED"))           return make_unique<Command>(Command::Type::BONDED);
		else if (!strcmp(rxCmd, "DISCONNECT"))       return make_unique<Command>(Command::Type::DISCONNECT);
		else if (!strcmp(rxCmd, "ERR_CONNPARAM"))    return make_unique<Command>(Command::Type::ERR_CONNPARAM);
		else if (!strcmp(rxCmd, "ERR_MEMORY"))       return make_unique<Command>(Command::Type::ERR_MEMORY);
		else if (!strcmp(rxCmd, "ERR_READ"))         return make_unique<Command>(Command::Type::ERR_READ);
		else if (!strcmp(rxCmd, "ERR_RMT_CMD"))      return make_unique<Command>(Command::Type::ERR_RMT_CMD);
		else if (!strcmp(rxCmd, "ERR_SEC"))          return make_unique<Command>(Command::Type::ERR_SEC);
		else if (!strcmp(rxCmd, "KEY_REQ"))          return make_unique<Command>(Command::Type::KEY_REQ);
		else if (!strcmp(rxCmd, "PIO1H"))            return make_unique<Command>(Command::Type::PIO1H);
		else if (!strcmp(rxCmd, "PIO1L"))            return make_unique<Command>(Command::Type::PIO1L);
		else if (!strcmp(rxCmd, "PIO2H"))            return make_unique<Command>(Command::Type::PIO2H);
		else if (!strcmp(rxCmd, "PIO2L"))            return make_unique<Command>(Command::Type::PIO2L);
		else if (!strcmp(rxCmd, "PIO3H"))            return make_unique<Command>(Command::Type::PIO3H);
		else if (!strcmp(rxCmd, "PIO3L"))            return make_unique<Command>(Command::Type::PIO3L);
		else if (!strcmp(rxCmd, "RE_DISCV"))         return make_unique<Command>(Command::Type::RE_DISCV);
		else if (!strcmp(rxCmd, "REBOOT"))           return make_unique<Command>(Command::Type::REBOOT);
		else if (!strcmp(rxCmd, "RMT_CMD_OFF"))      return make_unique<Command>(Command::Type::RMT_CMD_OFF);
		else if (!strcmp(rxCmd, "RMT_CMD_ON"))       return make_unique<Command>(Command::Type::RMT_CMD_ON);
		else if (!strcmp(rxCmd, "SECURED"))          return make_unique<Command>(Command::Type::SECURED);
		else if (!strcmp(rxCmd, "STREAM_OPEN"))      return make_unique<Command>(Command::Type::STREAM_OPEN);
		else if (!strcmp(rxCmd, "TMR1"))             return make_unique<Command>(Command::Type::TMR1);
		else if (!strcmp(rxCmd, "TMR2"))             return make_unique<Command>(Command::Type::TMR2);
		else if (!strcmp(rxCmd, "TMR3"))             return make_unique<Command>(Command::Type::TMR3);

		// --- Parameterized messages ---
		else if (strncmp(rxCmd, "CONN_PARAM,", 10) == 0) {
			int interval, latency, timeout;
			if (sscanf(rxCmd, "CONN_PARAM,%d,%d,%d", &interval, &latency, &timeout) == 3)
				return make_unique<ConnParamCommand>(interval, latency, timeout);
		}
		else if (strncmp(rxCmd, "CONNECT", 7) == 0) {
			int connected; char addr[32];
			if (sscanf(rxCmd, "CONNECT,%d,%31[^%%]", &connected, addr) == 2)
				return make_unique<ConnectCommand>(connected, addr);
		}
		else if (strncmp(rxCmd, "KEY:", 4) == 0) {
			int key;
			if (sscanf(rxCmd, "KEY:%d", &key) == 1)
				return make_unique<KeyCommand>(key);
		}
		else if (strncmp(rxCmd, "INDI,", 4) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "INDI,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<IndiCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "NOTI,", 5) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "NOTI,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<NotiCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "RV,", 3) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "RV,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<RvCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "S_RUN:", 6) == 0) {
			char cmd[64];
			if (sscanf(rxCmd, "S_RUN:%63[^%%]", cmd) == 1)
				return make_unique<SRunCommand>(cmd);
		}
		else if (strncmp(rxCmd, "WC,", 3) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "WC,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<WcCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "WV,", 3) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "WV,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<WvCommand>(hdl, hex);
		}

		// --- Advertisement messages ---
		else {
			char addr[32], name[32], uuids[64], hex[128];
			int connected, rssi;

			if (sscanf(rxCmd, "%31[^,],%d,%31[^,],%63[^,],%d", addr, &connected, name, uuids, &rssi) == 5) {
				return make_unique<AdvConnectableCommand>(addr, connected, name, uuids, rssi);
			}
			else if (strstr(rxCmd, ",Brcst,") != nullptr) {
				if (sscanf(rxCmd, "%31[^,],%d,%d,Brcst,%127[^%]", addr, &connected, &rssi, hex) == 4)
					return make_unique<AdvNonConnectableCommand>(addr, connected, rssi, hex);
			}
		}

		// If no match was found, return an 'UNKNOWN' command
		return make_unique<Command>(Command::Type::UNKNOWN);
	}


	uint8_t _send_string(std::string data) {
		if(data.length() > BUFF_SIZE) return 1;
		if(DMA1_Channel2->CCR & DMA_CCR_EN) return 1;

		memcpy(_txBuffer, data.c_str(), data.length());

		stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(&_txBuffer[0]));
		stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), data.length());
		stmcpp::reg::set(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);

		return 0;
	}

	uint8_t _send_char(char c) {
		if(DMA1_Channel2->CCR & DMA_CCR_EN) return 1;

		_txBuffer[0] = c;
		stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(&_txBuffer[0]));
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

		// Set up TX dma
        stmcpp::reg::write(std::ref(DMA1_Channel2->CCR), 
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
			(0b1 << DMA_CCR_DIR_Pos)   | // Read from memory
            (0b1 << DMA_CCR_TCIE_Pos)   // Transfer complete interrupt enable
            
        );

        // Set up the DMA memory and peripheral
        stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(&_txBuffer[0]));
        stmcpp::reg::write(std::ref(DMA1_Channel2->CPAR), reinterpret_cast<uint32_t>(&USART2->TDR));
        stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), 1);
        NVIC_EnableIRQ(DMA1_Channel2_IRQn);

        // Set up DMAMUX routing (DMAMUX channels map to 6-11 to DMA2 1-6, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel1->CCR), (27 << DMAMUX_CxCR_DMAREQ_ID_Pos));

		// Set up USART2
		stmcpp::reg::write(std::ref(USART2->BRR), 144000000 / 115200); // Assuming 144MHz clock, set baud rate to 115200
		stmcpp::reg::write(std::ref(USART2->CR1), 
			(0b1 << USART_CR1_TE_Pos) | // Transmitter enable
			(0b1 << USART_CR1_RE_Pos) | // Receiver enable
			(0b1 << USART_CR1_RXNEIE_Pos)  // RX not empty interrupt enable
		);

		stmcpp::reg::write(std::ref(USART2->CR3), 
			(0b1 << USART_CR3_DMAT_Pos)    // DMA transmitter enable
		);

		NVIC_EnableIRQ(USART2_IRQn);

		// Enable USART2
		stmcpp::reg::set(std::ref(USART2->CR1), USART_CR1_UE);

		stmcpp::systick::waitBlocking(100_ms);

		// Bring RN487x out of reset
		rst_n.set();
       
		return 0;
	}

	bool isCommandAvailable() {
        return !_commandTextQueue.empty();
    }

    std::unique_ptr<Command> getCommand() {
        if (_commandTextQueue.empty()) {
            return nullptr; // No command string available
        }
        
        // Move the command string from the queue to a local variable
        std::string cmdText = std::move(_commandTextQueue.front());
        _commandTextQueue.pop_front();
        
        // Parse the string and return the resulting command object
        return _build_command_from_string(cmdText);
    }



	extern "C" void DMA1_Channel2_IRQHandler(){
		// Disable TX DMA
		if(stmcpp::reg::read(std::ref(DMA1->ISR)) & DMA_ISR_TCIF2){
			stmcpp::reg::clear(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);
            stmcpp::reg::set(std::ref(DMA1->IFCR), DMA_IFCR_CTCIF2); 
        }
	}

	extern "C" void USART2_IRQHandler() {
		if(stmcpp::reg::read(std::ref(USART2->ISR)) & USART_ISR_RXNE){
			char c = static_cast<char>(USART2->RDR);

			if (c == '%' && !_receivingCommand){
				// Start reception of a command
				_receivingCommand = true;
				_rxCommandIndex = 0;
			}else if (_receivingCommand && c != '%'){
				// Fill the command buffer with the command
				_rxCommandBuffer[_rxCommandIndex++] = c;
				
			}else if (_receivingCommand && c == '%'){
				_rxCommandBuffer[_rxCommandIndex++] = '\0';
				_receivingCommand = false;
				_commandTextQueue.push_back(string(_rxCommandBuffer));
				// Process command
				//_parse_command();
				_rxCommandIndex = 0;
			}

        }
	}
}

