#include "protocol/codec.hpp"
#include "protocol/crc8.hpp"
#include "protocol/constants.hpp"

namespace protocol {

	// Zapíše escapovaný bajt do výstupního bufferu, vrátí počet zapsaných bajtů
	static uint16_t stuffByte(uint8_t byte, uint8_t *out) {
		switch (byte) {
			case FRAME_START:
				out[0] = ESC; out[1] = ESC_FRAME_START; return 2;
			case FRAME_END:
				out[0] = ESC; out[1] = ESC_FRAME_END; return 2;
			case ESC:
				out[0] = ESC; out[1] = ESC_ESC; return 2;
			case FORBIDDEN_DOLLAR:
				out[0] = ESC; out[1] = ESC_DOLLAR; return 2;
			case FORBIDDEN_PERCENT:
				out[0] = ESC; out[1] = ESC_PERCENT; return 2;
			default:
				out[0] = byte; return 1;
		}
	}

	// Vrátí originální bajt z escape sekvence, nebo -1 při chybě
	static int unstuffByte(uint8_t escaped) {
		switch (escaped) {
			case ESC_FRAME_START: return FRAME_START;
			case ESC_FRAME_END:   return FRAME_END;
			case ESC_ESC:         return ESC;
			case ESC_DOLLAR:      return FORBIDDEN_DOLLAR;
			case ESC_PERCENT:     return FORBIDDEN_PERCENT;
			default:              return -1;
		}
	}

	uint16_t encode(const Packet &pkt, uint8_t *output) {
		uint16_t pos = 0;

		// Sestavení neescapovaného obsahu pro CRC výpočet
		uint8_t length = HEADER_SIZE + pkt.payloadLen;
		uint8_t raw[1 + HEADER_SIZE + MAX_PAYLOAD_SIZE]; // Length + header + payload
		raw[0] = length;
		raw[1] = pkt.source;
		raw[2] = pkt.dest;
		raw[3] = pkt.packetId;
		raw[4] = static_cast<uint8_t>(pkt.msgType);
		for (uint8_t i = 0; i < pkt.payloadLen; ++i) {
			raw[5 + i] = pkt.payload[i];
		}
		uint8_t rawLen = 1 + HEADER_SIZE + pkt.payloadLen;
		uint8_t checksum = crc8(raw, rawLen);

		// Zápis rámce
		output[pos++] = FRAME_START;

		// Escapování Length .. Payload
		for (uint8_t i = 0; i < rawLen; ++i) {
			pos += stuffByte(raw[i], &output[pos]);
		}

		// Escapování checksum
		pos += stuffByte(checksum, &output[pos]);

		output[pos++] = FRAME_END;

		return pos;
	}

	bool decodeFrame(const uint8_t *raw, uint8_t len, Packet &pkt) {
		// Nejdříve un-stuff celý obsah
		uint8_t unstuffed[1 + HEADER_SIZE + MAX_PAYLOAD_SIZE + 1]; // Length + header + payload + CRC
		uint8_t uLen = 0;

		for (uint8_t i = 0; i < len; ++i) {
			if (raw[i] == ESC) {
				if (i + 1 >= len) return false;
				int val = unstuffByte(raw[++i]);
				if (val < 0) return false;
				unstuffed[uLen++] = static_cast<uint8_t>(val);
			} else {
				unstuffed[uLen++] = raw[i];
			}
		}

		// Minimální délka: Length(1) + Header(4) + CRC(1) = 6
		if (uLen < 6) return false;

		uint8_t length = unstuffed[0];
		uint8_t payloadLen = length - HEADER_SIZE;

		// Kontrola konzistence
		if (length < HEADER_SIZE) return false;
		if (payloadLen > MAX_PAYLOAD_SIZE) return false;
		if (uLen != static_cast<uint8_t>(1 + length + 1)) return false; // Length byte + content + CRC

		// CRC kontrola: přes Length .. Payload (vše kromě posledního bajtu = CRC)
		uint8_t expectedCrc = crc8(unstuffed, uLen - 1);
		if (expectedCrc != unstuffed[uLen - 1]) return false;

		// Naplnění struktury
		pkt.source   = unstuffed[1];
		pkt.dest     = unstuffed[2];
		pkt.packetId = unstuffed[3];
		pkt.msgType  = static_cast<MsgType>(unstuffed[4]);
		pkt.payloadLen = payloadLen;
		for (uint8_t i = 0; i < payloadLen; ++i) {
			pkt.payload[i] = unstuffed[5 + i];
		}

		return true;
	}

} // namespace protocol
