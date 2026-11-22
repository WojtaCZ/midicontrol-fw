#include "settings.hpp"
#include "debug.hpp"
#include "logic/settings_store.hpp"
#include <stm32g431xx.h>
#include <cmsis_compiler.h>
#include <cstring>

namespace settings {

	static constexpr uint32_t SETTINGS_MAGIC = 0xBEEFCAFE;
	static constexpr uint8_t SETTINGS_VERSION = 2; // RAM struct version; the flash format is settingsstore (v3 slots)

	// Last flash page (page 63) — 2KB pages on the STM32G4
	static constexpr uint32_t SETTINGS_PAGE = 63;
	static constexpr uint32_t SETTINGS_ADDR = 0x0801F800;

	static SettingsData ramCopy;

	static void loadDefaults() {
		ramCopy.magic = SETTINGS_MAGIC;
		ramCopy.version = SETTINGS_VERSION;
		ramCopy.ledMasterEnable = true;
		ramCopy.midiRoleId = true;
		ramCopy._pad = 0;
		std::memset(ramCopy.remoteAddr, 0, sizeof(ramCopy.remoteAddr));
	}

	// RAM copy <-> store record (the RAM layout stays as is: tools/basetest reads remoteAddr at offset 8)
	static settingsstore::Data toData() {
		settingsstore::Data d;
		d.ledMasterEnable = ramCopy.ledMasterEnable;
		d.midiRoleId = ramCopy.midiRoleId;
		std::memcpy(d.remoteAddr, ramCopy.remoteAddr, sizeof(d.remoteAddr));
		return d;
	}

	static const uint8_t *flashPage() {
		return reinterpret_cast<const uint8_t *>(SETTINGS_ADDR);
	}

	void init() {
		loadDefaults();
		const settingsstore::LoadResult r = settingsstore::load(flashPage());
		if (r.source == settingsstore::Source::None) {
			Debug::log(Debug::Level::WARNING, "set", "no valid settings, defaults");
			return;
		}
		ramCopy.ledMasterEnable = r.data.ledMasterEnable;
		ramCopy.midiRoleId = r.data.midiRoleId;
		std::memcpy(ramCopy.remoteAddr, r.data.remoteAddr, sizeof(ramCopy.remoteAddr));
		if (r.source == settingsstore::Source::V1) {
			Debug::log(Debug::Level::INFO, "set", "migrated v1 -> v3 (remote must be paired)");
		} else {
			Debug::log(Debug::Level::INFO, "set", "loaded v%u, remote %s", r.source == settingsstore::Source::V3 ? 3u : 2u,
					   ramCopy.remoteAddr[0] ? ramCopy.remoteAddr : "-");
		}
	}

	static constexpr uint32_t FLASH_ERR_MASK = FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR |
											   FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR;

	static void flashUnlock() {
		if (FLASH->CR & FLASH_CR_LOCK) {
			FLASH->KEYR = 0x45670123;
			FLASH->KEYR = 0xCDEF89AB;
		}
	}

	static void flashLock() {
		FLASH->CR |= FLASH_CR_LOCK;
	}

	static void flashWaitBusy() {
		while (FLASH->SR & FLASH_SR_BSY) {}
	}

	// true = no error flag; the flags are cleared either way
	static bool flashTakeStatus() {
		const bool ok = (FLASH->SR & FLASH_ERR_MASK) == 0;
		FLASH->SR = FLASH->SR & (FLASH_ERR_MASK | FLASH_SR_EOP);
		return ok;
	}

	static bool flashErasePage(uint32_t page) {
		flashWaitBusy();
		flashTakeStatus();
		// Page erase (single bank)
		FLASH->CR &= ~FLASH_CR_PNB_Msk;
		FLASH->CR |= (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER;
		FLASH->CR |= FLASH_CR_STRT;
		flashWaitBusy();
		FLASH->CR &= ~FLASH_CR_PER;
		return flashTakeStatus();
	}

	static bool flashProgramDoubleWord(uint32_t addr, uint64_t data) {
		flashWaitBusy();
		flashTakeStatus();
		FLASH->CR |= FLASH_CR_PG;

		// STM32G4 flash programming in 64-bit words
		*reinterpret_cast<volatile uint32_t *>(addr) = static_cast<uint32_t>(data);
		*reinterpret_cast<volatile uint32_t *>(addr + 4) = static_cast<uint32_t>(data >> 32);

		flashWaitBusy();
		FLASH->CR &= ~FLASH_CR_PG;
		return flashTakeStatus();
	}

	// Program one 32-byte slot and read it back; false on an error flag or a mismatch.
	static bool writeSlot(int index, const uint8_t *slot) {
		const uint32_t addr = SETTINGS_ADDR + static_cast<uint32_t>(index) * settingsstore::SLOT_SIZE;
		bool ok = true;
		for (uint32_t i = 0; i < settingsstore::SLOT_SIZE && ok; i += 8) {
			uint64_t word;
			std::memcpy(&word, slot + i, 8);
			ok = flashProgramDoubleWord(addr + i, word);
		}
		return ok && std::memcmp(flashPage() + static_cast<size_t>(index) * settingsstore::SLOT_SIZE, slot, settingsstore::SLOT_SIZE) == 0;
	}

	// C8: append a record in the next blank slot (no erase); the page is erased only when full. A failed or torn slot is
	// skipped (the next one is tried, twice at most). The settings stay in RAM on failure.
	void save() {
		const settingsstore::Data data = toData();
		const settingsstore::LoadResult cur = settingsstore::load(flashPage());
		if (cur.source == settingsstore::Source::V3 && cur.data == data) return; // nothing new: no flash wear

		uint8_t slot[settingsstore::SLOT_SIZE];
		settingsstore::encode(data, slot);

		uint32_t primask = __get_PRIMASK();
		__disable_irq();
		flashUnlock();

		bool ok = false;
		int idx = cur.nextFree;
		for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
			if (idx < 0 || idx >= static_cast<int>(settingsstore::SLOT_COUNT)) {
				if (!flashErasePage(SETTINGS_PAGE)) break;
				idx = 0;
			}
			ok = writeSlot(idx, slot);
			if (!ok) ++idx; // the slot is dirty now: move on
		}

		flashLock();
		if (!primask) {
			__enable_irq();
		}
		if (ok) {
			Debug::log(Debug::Level::INFO, "set", "saved, remote %s", ramCopy.remoteAddr[0] ? ramCopy.remoteAddr : "-");
		} else {
			Debug::log(Debug::Level::ERROR, "set", "save FAILED (flash error), settings kept in RAM only");
		}
	}

	bool ledMasterEnable() {
		return ramCopy.ledMasterEnable;
	}

	void setLedMasterEnable(bool value) {
		ramCopy.ledMasterEnable = value;
	}

	bool midiRoleIdentifier() {
		return ramCopy.midiRoleId;
	}

	void setMidiRoleIdentifier(bool value) {
		ramCopy.midiRoleId = value;
	}

	const char *remoteAddr() {
		return ramCopy.remoteAddr;
	}

	void setRemoteAddr(const char *addr) {
		std::strncpy(ramCopy.remoteAddr, addr, sizeof(ramCopy.remoteAddr) - 1);
		ramCopy.remoteAddr[sizeof(ramCopy.remoteAddr) - 1] = '\0';
	}

} // namespace settings
