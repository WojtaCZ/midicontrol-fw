#ifndef PROTOCOL_TYPES_HPP
#define PROTOCOL_TYPES_HPP

#include <cstdint>
#include "protocol/constants.hpp"

namespace protocol {

	enum class MsgType : uint8_t {
		ACK                = 0x01,
		NAK                = 0x02,

		PLAY_SONG          = 0x10,
		STOP_SONG          = 0x11,
		RECORD_SONG        = 0x12,

		GET_SONG_LIST      = 0x20,
		SONG_LIST_RESP     = 0x21,

		SIGNAL_PLAYING     = 0x30,
		SIGNAL_RECORDING   = 0x31,
		SIGNAL_STOPPED     = 0x32,

		SET_CURRENT        = 0x40,

		GET_STATUS         = 0x50,
		STATUS_RESP        = 0x51,

		KEEPALIVE          = 0x60,
		KEEPALIVE_ACK      = 0x61,
	};

	enum class NakReason : uint8_t {
		UNKNOWN_MSG        = 0x01,
		INVALID_PAYLOAD    = 0x02,
		CRC_ERROR          = 0x03,
		BUSY               = 0x04,
	};

	struct Packet {
		uint8_t source;
		uint8_t dest;
		uint8_t packetId;
		MsgType msgType;
		uint8_t payload[MAX_PAYLOAD_SIZE];
		uint8_t payloadLen;
	};

} // namespace protocol

#endif
