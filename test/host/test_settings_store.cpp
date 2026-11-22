// C8: settings slot store (src/logic/settings_store.*), run against a RAM flash model.
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

#include "settings_store.hpp"

using namespace settingsstore;

namespace {
	struct Flash {
		std::vector<uint8_t> page = std::vector<uint8_t>(PAGE_SIZE, 0xFF);
		// save() as the firmware does it; cut = number of bytes programmed before the power fails (-1 = no cut)
		void save(const Data &d, int cut = -1) {
			LoadResult r = load(page.data());
			if (r.nextFree < 0) {
				std::fill(page.begin(), page.end(), 0xFF);
				r.nextFree = 0;
			}
			uint8_t slot[SLOT_SIZE];
			encode(d, slot);
			const size_t n = cut < 0 ? SLOT_SIZE : static_cast<size_t>(cut);
			std::memcpy(&page[static_cast<size_t>(r.nextFree) * SLOT_SIZE], slot, n);
		}
	};
	Data make(bool led, bool midi, const char *addr) {
		Data d;
		d.ledMasterEnable = led;
		d.midiRoleId	  = midi;
		std::strncpy(d.remoteAddr, addr, ADDR_LEN - 1);
		return d;
	}
}

TEST(SettingsStore, BlankPageIsNone) {
	Flash f;
	const LoadResult r = load(f.page.data());
	EXPECT_EQ(r.source, Source::None);
	EXPECT_EQ(r.nextFree, 0);
}

TEST(SettingsStore, SaveLoadRoundTripAndNewestWins) {
	Flash f;
	f.save(make(true, true, "001122334455"));
	f.save(make(false, true, "001122334455"));
	const LoadResult r = load(f.page.data());
	EXPECT_EQ(r.source, Source::V3);
	EXPECT_FALSE(r.data.ledMasterEnable);
	EXPECT_STREQ(r.data.remoteAddr, "001122334455");
	EXPECT_EQ(r.nextFree, 2);
}

// A power cut after any number of programmed bytes leaves the previous record readable.
TEST(SettingsStore, PowerCutAtEveryStepKeepsPreviousRecord) {
	for (int cut = 0; cut < static_cast<int>(SLOT_SIZE); ++cut) {
		Flash f;
		f.save(make(true, false, "AABBCCDDEEFF"));
		f.save(make(false, true, "112233445566"), cut);
		const LoadResult r = load(f.page.data());
		ASSERT_EQ(r.source, Source::V3) << "cut " << cut;
		EXPECT_TRUE(r.data.ledMasterEnable) << "cut " << cut;
		EXPECT_FALSE(r.data.midiRoleId) << "cut " << cut;
		EXPECT_STREQ(r.data.remoteAddr, "AABBCCDDEEFF") << "cut " << cut;
		// the torn slot is skipped by the next save (it is not blank unless nothing was written)
		f.save(make(false, true, "112233445566"));
		EXPECT_FALSE(load(f.page.data()).data.ledMasterEnable) << "cut " << cut;
	}
}

TEST(SettingsStore, BadCrcIsIgnored) {
	Flash f;
	f.save(make(true, true, "AABBCCDDEEFF"));
	f.save(make(false, false, "AABBCCDDEEFF"));
	f.page[SLOT_SIZE + 5] ^= 0x01; // corrupt the newest slot's payload
	const LoadResult r = load(f.page.data());
	EXPECT_EQ(r.source, Source::V3);
	EXPECT_TRUE(r.data.ledMasterEnable) << "falls back to the previous valid slot";
}

TEST(SettingsStore, FullPageWrapsWithErase) {
	Flash f;
	for (size_t i = 0; i < SLOT_COUNT; ++i) f.save(make(i & 1, true, "AABBCCDDEEFF"));
	EXPECT_EQ(load(f.page.data()).nextFree, -1);
	f.save(make(true, false, "FFEEDDCCBBAA")); // erase + slot 0
	const LoadResult r = load(f.page.data());
	EXPECT_EQ(r.nextFree, 1);
	EXPECT_STREQ(r.data.remoteAddr, "FFEEDDCCBBAA");
	EXPECT_FALSE(r.data.midiRoleId);
}

TEST(SettingsStore, MigratesV2AndV1) {
	Flash f;
	// v2 record: magic, version 2, led=0, midi=1, pad, remoteAddr
	const uint32_t m = V2_MAGIC;
	std::memcpy(&f.page[0], &m, 4);
	f.page[4] = 2; f.page[5] = 0; f.page[6] = 1; f.page[7] = 0;
	std::memcpy(&f.page[8], "001122334455", 13);
	LoadResult r = load(f.page.data());
	EXPECT_EQ(r.source, Source::V2);
	EXPECT_FALSE(r.data.ledMasterEnable);
	EXPECT_STREQ(r.data.remoteAddr, "001122334455");
	EXPECT_EQ(r.nextFree, 1) << "the first v3 slot goes after the legacy record, no erase";
	f.save(make(true, true, "001122334455"));
	EXPECT_EQ(load(f.page.data()).source, Source::V3);

	Flash g;
	std::memcpy(&g.page[0], &m, 4);
	g.page[4] = 1; g.page[5] = 1; g.page[6] = 0;
	r = load(g.page.data());
	EXPECT_EQ(r.source, Source::V1);
	EXPECT_FALSE(r.data.midiRoleId);
	EXPECT_EQ(r.data.remoteAddr[0], '\0');
}

TEST(SettingsStore, Crc32KnownVector) {
	const uint8_t v[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	EXPECT_EQ(crc32(v, sizeof(v)), 0xCBF43926u);
}
