#ifndef CRC32_HPP
#define CRC32_HPP

#include <cstddef>
#include <cstdint>

namespace util {

	// CRC-32 (IEEE 802.3, zrcadlená 0xEDB88320, init 0xFFFFFFFF, výstup negovaný) = Python zlib.crc32.
	// Bez tabulky: hlavičky v no-init RAM jsou malé a počítají se zřídka.
	inline uint32_t crc32(const void *data, size_t len) {
		const uint8_t *p   = static_cast<const uint8_t *>(data);
		uint32_t	   crc = 0xFFFFFFFF;
		for (size_t i = 0; i < len; ++i) {
			crc ^= p[i];
			for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
		}
		return ~crc;
	}

} // namespace util

#endif
