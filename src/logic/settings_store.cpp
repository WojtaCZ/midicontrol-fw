#include "settings_store.hpp"
#include <cstring>

namespace settingsstore {

	// Slot layout: [0..3] magic, [4] version (3), [5] led, [6] midi, [7] reserved,
	// [8..20] remoteAddr[13], [21..27] reserved (0xFF), [28..31] CRC32 of bytes 0..27.
	static constexpr size_t CRC_OFFSET = 28;

	bool Data::operator==(const Data &o) const {
		return ledMasterEnable == o.ledMasterEnable && midiRoleId == o.midiRoleId && std::memcmp(remoteAddr, o.remoteAddr, ADDR_LEN) == 0;
	}

	uint32_t crc32(const uint8_t *data, size_t len) {
		uint32_t c = 0xFFFFFFFFu;
		for (size_t i = 0; i < len; ++i) {
			c ^= data[i];
			for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		}
		return ~c;
	}

	static void put32(uint8_t *p, uint32_t v) {
		p[0] = static_cast<uint8_t>(v);
		p[1] = static_cast<uint8_t>(v >> 8);
		p[2] = static_cast<uint8_t>(v >> 16);
		p[3] = static_cast<uint8_t>(v >> 24);
	}

	static uint32_t get32(const uint8_t *p) {
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
	}

	void encode(const Data &d, uint8_t *slot) {
		std::memset(slot, 0xFF, SLOT_SIZE);
		put32(slot, SLOT_MAGIC);
		slot[4] = 3;
		slot[5] = d.ledMasterEnable ? 1 : 0;
		slot[6] = d.midiRoleId ? 1 : 0;
		std::memcpy(slot + 8, d.remoteAddr, ADDR_LEN);
		slot[8 + ADDR_LEN - 1] = '\0';
		put32(slot + CRC_OFFSET, crc32(slot, CRC_OFFSET));
	}

	bool decode(const uint8_t *slot, Data &out) {
		if (get32(slot) != SLOT_MAGIC || slot[4] != 3) return false;
		if (get32(slot + CRC_OFFSET) != crc32(slot, CRC_OFFSET)) return false;
		out.ledMasterEnable = slot[5] != 0;
		out.midiRoleId		= slot[6] != 0;
		std::memcpy(out.remoteAddr, slot + 8, ADDR_LEN);
		out.remoteAddr[ADDR_LEN - 1] = '\0';
		return true;
	}

	bool blank(const uint8_t *slot) {
		for (size_t i = 0; i < SLOT_SIZE; ++i) {
			if (slot[i] != 0xFF) return false;
		}
		return true;
	}

	LoadResult load(const uint8_t *page) {
		LoadResult r;
		r.nextFree = -1;
		// First blank slot (records are appended, so everything before it was written or torn)
		for (size_t i = 0; i < SLOT_COUNT; ++i) {
			if (blank(page + i * SLOT_SIZE)) {
				r.nextFree = static_cast<int>(i);
				break;
			}
		}
		// Newest valid v3 slot: the highest index wins
		for (size_t i = SLOT_COUNT; i-- > 0;) {
			Data d;
			if (decode(page + i * SLOT_SIZE, d)) {
				r.source = Source::V3;
				r.data	 = d;
				return r;
			}
		}
		// Legacy single record at the page start: v2 = magic u32, version u8 = 2, led, midi, pad, remoteAddr[13]; v1 = first 8 bytes
		if (get32(page) == V2_MAGIC && (page[4] == 1 || page[4] == 2)) {
			r.data.ledMasterEnable = page[5] != 0;
			r.data.midiRoleId	   = page[6] != 0;
			if (page[4] == 2) {
				std::memcpy(r.data.remoteAddr, page + 8, ADDR_LEN);
				r.data.remoteAddr[ADDR_LEN - 1] = '\0';
				r.source = Source::V2;
			} else {
				r.source = Source::V1;
			}
		}
		return r;
	}

} // namespace settingsstore
