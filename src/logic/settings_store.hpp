// Power-cut-safe settings record format (C8, Q2-9, "settings v3"). Hardware-independent: the flash page is a byte array.
//
// The settings page (2 KB) holds up to 64 slots of 32 bytes, appended one after the other. A save programs the first
// blank slot (no erase), so a power cut can only damage the slot being written; the previous records stay intact.
// Load = the last slot (highest index) whose magic and CRC are valid. The page is erased only when all slots are used
// (about every 64th save); that one erase is the only window in which a power cut loses the settings.
// A v1/v2 page (legacy single record at the page start) is still read: the first v3 slot goes after it.
#pragma once
#include <cstddef>
#include <cstdint>

namespace settingsstore {

	constexpr size_t   PAGE_SIZE   = 2048;
	constexpr size_t   SLOT_SIZE   = 32;
	constexpr size_t   SLOT_COUNT  = PAGE_SIZE / SLOT_SIZE;
	constexpr uint32_t SLOT_MAGIC  = 0x33544553; // "SET3"
	constexpr uint32_t V2_MAGIC	   = 0xBEEFCAFE;
	constexpr size_t   ADDR_LEN	   = 13;

	struct Data {
		bool   ledMasterEnable = true;
		bool   midiRoleId	   = true;
		char   remoteAddr[ADDR_LEN] = {};
		bool operator==(const Data &o) const;
	};

	enum class Source : uint8_t { None, V3, V2, V1 };

	struct LoadResult {
		LoadResult() : data() {}
		Source source	= Source::None;
		Data   data;
		int	   nextFree = 0; // index of the first blank slot, -1 = page full (erase before the next save)
	};

	uint32_t crc32(const uint8_t *data, size_t len);

	// Encode one slot (SLOT_SIZE bytes).
	void encode(const Data &d, uint8_t *slot);
	// true when the slot carries a valid v3 record.
	bool decode(const uint8_t *slot, Data &out);
	// true when every byte of the slot is 0xFF (erased).
	bool blank(const uint8_t *slot);

	// Scan the page (PAGE_SIZE bytes).
	LoadResult load(const uint8_t *page);

} // namespace settingsstore
