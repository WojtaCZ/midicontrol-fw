#include "protocol/parser.hpp"
#include "protocol/codec.hpp"

namespace protocol {

	bool Parser::feed(uint8_t byte) {
		switch (_state) {
			case State::IDLE:
				if (byte == FRAME_START) {
					_state = State::RECEIVING;
					_bufferIndex = 0;
				}
				// Ostatní bajty mimo rámec ignorujeme
				return false;

			case State::RECEIVING:
				if (byte == FRAME_END) {
					_state = State::IDLE;
					// Zkusíme dekódovat obsah bufferu
					if (_bufferIndex > 0 && decodeFrame(_buffer, _bufferIndex, _packet)) {
						return true;
					}
					// Neplatný rámec — zahodíme
					return false;
				}

				if (byte == FRAME_START) {
					// Nový rámec začíná — zahodíme rozpracovaný
					_bufferIndex = 0;
					return false;
				}

				// Normální datový bajt
				if (_bufferIndex < MAX_FRAME_SIZE) {
					_buffer[_bufferIndex++] = byte;
				} else {
					// Přetečení — resetujeme
					_state = State::IDLE;
					_bufferIndex = 0;
				}
				return false;
		}

		return false;
	}

	void Parser::reset() {
		_state = State::IDLE;
		_bufferIndex = 0;
	}

} // namespace protocol
