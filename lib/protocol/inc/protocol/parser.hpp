#ifndef PROTOCOL_PARSER_HPP
#define PROTOCOL_PARSER_HPP

#include <cstdint>
#include "protocol/types.hpp"
#include "protocol/constants.hpp"

namespace protocol {

	class Parser {
	public:
		// Přijme jeden bajt z linky. Vrací true, když je kompletní rámec připraven.
		bool feed(uint8_t byte);

		// Vrátí naposledy přijatý a dekódovaný paket.
		const Packet &getPacket() const { return _packet; }

		// Reset parseru do výchozího stavu
		void reset();

	private:
		enum class State : uint8_t {
			IDLE,       // Čekání na FRAME_START
			RECEIVING,  // Příjem dat mezi FRAME_START a FRAME_END
		};

		State _state = State::IDLE;
		uint8_t _buffer[MAX_FRAME_SIZE];
		uint16_t _bufferIndex = 0;
		Packet _packet = {};
	};

} // namespace protocol

#endif
