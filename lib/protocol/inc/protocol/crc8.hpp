#ifndef PROTOCOL_CRC8_HPP
#define PROTOCOL_CRC8_HPP

#include <cstdint>
#include <cstddef>

namespace protocol {

	// CRC-8/MAXIM (poly 0x31, init 0x00)
	uint8_t crc8(const uint8_t *data, size_t len);

} // namespace protocol

#endif
