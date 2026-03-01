#ifndef PROTOCOL_CODEC_HPP
#define PROTOCOL_CODEC_HPP

#include <cstdint>
#include "protocol/types.hpp"

namespace protocol {

	// Zakóduje paket do drátového formátu (s escapováním a rámcováním).
	// Vrací počet zapsaných bajtů do output bufferu.
	uint16_t encode(const Packet &pkt, uint8_t *output);

	// Dekóduje surový rámec (bez FRAME_START/FRAME_END) zpět na paket.
	// raw ukazuje na data mezi FRAME_START a FRAME_END (escapovaná).
	// Vrací true při úspěšné validaci CRC.
	bool decodeFrame(const uint8_t *raw, uint8_t len, Packet &pkt);

} // namespace protocol

#endif
