#include "midi.hpp"
#include "wdg.hpp"
#include "base.hpp"
#include "led.hpp"

#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <stm32g431xx.h>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/midi/midi_device.h>

#include <string.h>
#include "ringbuffer.hpp"
#include "debug.hpp"
#include "stmcpp/systick.hpp"

#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"

namespace midi {

	using namespace stmcpp::units;

	struct UsbPacket {
		uint8_t data[4];
	};

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

	void send(uint8_t (&packet)[4]) {
		uint8_t size = messageSize(packet[0] & 0x0F); // Upper nibble = cable number
		wdg::kick(); // busy-waiting on the USART (~1 ms per message); a batch from the USB FIFO has several in a row
		for (uint8_t i = 1; i <= size; ++i) {
			// Wait until transmit data register is empty
			while (!(USART3->ISR & USART_ISR_TXE)) {}
			stmcpp::reg::write(std::ref(USART3->TDR), packet[i]);
		}

		// Wait for transmission complete
		while (!(USART3->ISR & USART_ISR_TC)) {}
	}

	// ── DIN input (organ) → USB ───────────────────────────────────────────
	// The parser runs in the ISR and assembles USB-MIDI packets into a ring buffer; they are
	// written to TinyUSB only by the main loop (process), because TinyUSB is not ISR-safe.
	static RingBuffer<UsbPacket, 64> _dinToUsb;

	static uint8_t _status = 0;        // Current (also running) status, 0 = none
	static uint8_t _dataNeeded = 0;    // Number of data bytes of the message
	static uint8_t _data[2] = {};
	static uint8_t _dataIdx = 0;
	static bool _inSysex = false;
	static uint8_t _sysex[3] = {};
	static uint8_t _sysexIdx = 0;

	static void _emit(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
		UsbPacket pkt = {{cin, b1, b2, b3}};
		if (!_dinToUsb.push(pkt)) Debug::logFromIsr(Debug::ISR_MIDI_OVERFLOW); // On overflow the packet is dropped
	}

	static uint8_t _dataBytesFor(uint8_t status) {
		switch (status & 0xF0) {
			case 0xC0: // Program change
			case 0xD0: // Channel pressure
				return 1;
			case 0xF0:
				return (status == 0xF2) ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0;
			default:
				return 2;
		}
	}

	static void _parseDinByte(uint8_t data) {
		// Real-time messages (F8-FF) can arrive at any time, even in the middle of a SysEx or another message,
		// and they interrupt nothing
		if (data >= 0xF8) {
			_emit(MIDI_CIN_1BYTE_DATA, data, 0, 0);
			return;
		}

		if (data == 0xF0) {
			_inSysex = true;
			_sysex[0] = data;
			_sysexIdx = 1;
			_status = 0;
			return;
		}

		if (data == 0xF7) {
			if (!_inSysex) return;
			_sysex[_sysexIdx++] = data;
			uint8_t cin = (_sysexIdx == 1) ? MIDI_CIN_SYSEX_END_1BYTE : (_sysexIdx == 2) ? MIDI_CIN_SYSEX_END_2BYTE : MIDI_CIN_SYSEX_END_3BYTE;
			_emit(cin, _sysex[0], _sysexIdx > 1 ? _sysex[1] : 0, _sysexIdx > 2 ? _sysex[2] : 0);
			_inSysex = false;
			_sysexIdx = 0;
			return;
		}

		if (data >= 0x80) {
			// Any other status terminates the SysEx (an incomplete one is dropped)
			_inSysex = false;
			_sysexIdx = 0;

			if (data == 0xF6) { // Tune request: single-byte system common
				_emit(MIDI_CIN_SYSEX_END_1BYTE, data, 0, 0);
				_status = 0;
				return;
			}
			if (data >= 0xF4) { // Undefined F4/F5
				_status = 0;
				return;
			}

			_status = data;
			_dataNeeded = _dataBytesFor(data);
			_dataIdx = 0;
			return;
		}

		// Data byte
		if (_inSysex) {
			_sysex[_sysexIdx++] = data;
			if (_sysexIdx == 3) {
				_emit(MIDI_CIN_SYSEX_START, _sysex[0], _sysex[1], _sysex[2]);
				_sysexIdx = 0;
			}
			return;
		}

		if (_status == 0) return; // Data without a status

		_data[_dataIdx++] = data;
		if (_dataIdx < _dataNeeded) return;

		uint8_t cin;
		if (_status >= 0xF0) {
			cin = (_dataNeeded == 2) ? MIDI_CIN_SYSCOM_3BYTE : MIDI_CIN_SYSCOM_2BYTE;
		} else {
			cin = _status >> 4; // Channel messages: CIN = upper nibble of the status
		}
		_emit(cin, _status, _data[0], _dataNeeded > 1 ? _data[1] : 0);
		_dataIdx = 0;

		// Running status applies only to channel messages, system common cancels it
		if (_status >= 0xF0) _status = 0;
	}

	// Receive handler for midi
	extern "C" void USART3_IRQHandler(void) {
		uint32_t isr = USART3->ISR;

		if (isr & USART_ISR_RXNE) {
			// Read the received byte (this also clears the RXNE flag)
			uint8_t data = static_cast<uint8_t>(stmcpp::reg::read(std::ref(USART3->RDR)));
			_parseDinByte(data);
			LED::notifyActivity(LED::PIXEL_MIDIA);
		}

		if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
			// Receive error: clear the flags and drop the message in progress
			USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
			Debug::logFromIsr(Debug::ISR_MIDI_UART_ERROR);
			_inSysex = false;
			_sysexIdx = 0;
			_dataIdx = 0;
		}
	}

	// ── USB output (main loop) ────────────────────────────────────────────
	static UsbPacket _echo[8];
	static uint8_t _echoCount = 0;
	static uint8_t _echoIdx = 0;
	static bool _dinSysexOpen = false;  // The start of a DIN SysEx has gone out to USB, but not the end yet
	static duration _lastDinPacket = 0_ms;

	bool queueUsbSysex(const uint8_t (*packets)[4], uint8_t count) {
		if (_echoCount != 0 || count > sizeof(_echo) / sizeof(_echo[0])) return false;
		for (uint8_t i = 0; i < count; ++i) {
			memcpy(_echo[i].data, packets[i], 4);
		}
		_echoIdx = 0;
		_echoCount = count;
		return true;
	}

	void process() {
		if (!tud_midi_mounted()) {
			// Nobody to send to — drop it, so old data is not sent after connecting
			_dinToUsb.clear();
			_echoCount = 0;
			_dinSysexOpen = false;
			return;
		}

		duration now = stmcpp::systick::getDuration();
		// An interrupted DIN SysEx (unplugged cable) must not block the display echo forever
		if (_dinSysexOpen && _dinToUsb.empty() && (now - _lastDinPacket) > 200_ms) _dinSysexOpen = false;

		while (true) {
			// Our own SysEx (display echo) must not be inserted in the middle of a DIN SysEx; once it
			// has started sending, it must finish before anything else
			if (_echoCount != 0 && (_echoIdx > 0 || !_dinSysexOpen)) {
				while (_echoIdx < _echoCount && tud_midi_packet_write(_echo[_echoIdx].data)) _echoIdx++;
				if (_echoIdx < _echoCount) return; // FIFO full, continue next time
				_echoCount = 0;
				_echoIdx = 0;
				continue;
			}

			UsbPacket *pkt = _dinToUsb.peek();
			if (!pkt) return;
			if (!tud_midi_packet_write(pkt->data)) return; // FIFO full, the packet stays in the queue
			LED::notifyActivity(LED::PIXEL_USB);

			uint8_t cin = pkt->data[0] & 0x0F;
			if (cin == MIDI_CIN_SYSEX_START) _dinSysexOpen = true;
			else if (cin != MIDI_CIN_1BYTE_DATA) _dinSysexOpen = false;
			_lastDinPacket = now;
			_dinToUsb.drop();
		}
	}

}
