#include "settings.hpp"
#include "debug.hpp"
#include <stm32g431xx.h>
#include <cmsis_compiler.h>
#include <cstring>

namespace settings {

	static constexpr uint32_t SETTINGS_MAGIC = 0xBEEFCAFE;
	static constexpr uint8_t SETTINGS_VERSION = 2;
	// Version 1: only the first 8 bytes (without remoteAddr)
	static constexpr uint32_t SETTINGS_V1_SIZE = 8;

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

	void init() {
		const auto *flash = reinterpret_cast<const SettingsData *>(SETTINGS_ADDR);
		if (flash->magic == SETTINGS_MAGIC && flash->version == SETTINGS_VERSION) {
			std::memcpy(&ramCopy, flash, sizeof(SettingsData));
			ramCopy.remoteAddr[sizeof(ramCopy.remoteAddr) - 1] = '\0';
			Debug::log(Debug::Level::INFO, "set", "loaded v%u, remote %s", SETTINGS_VERSION, ramCopy.remoteAddr[0] ? ramCopy.remoteAddr : "-");
		} else if (flash->magic == SETTINGS_MAGIC && flash->version == 1) {
			// Migration: the LED/MIDI settings are kept, the remote must be paired once from the menu
			loadDefaults();
			std::memcpy(&ramCopy, flash, SETTINGS_V1_SIZE);
			ramCopy.version = SETTINGS_VERSION;
			std::memset(ramCopy.remoteAddr, 0, sizeof(ramCopy.remoteAddr));
			Debug::log(Debug::Level::INFO, "set", "migrated v1 -> v%u (remote must be paired)", SETTINGS_VERSION);
		} else {
			loadDefaults();
			Debug::log(Debug::Level::WARNING, "set", "no valid settings, defaults");
		}
	}

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

	static void flashErasePage(uint32_t page) {
		flashWaitBusy();
		// Page erase (single bank)
		FLASH->CR &= ~FLASH_CR_PNB_Msk;
		FLASH->CR |= (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER;
		FLASH->CR |= FLASH_CR_STRT;
		flashWaitBusy();
		FLASH->CR &= ~FLASH_CR_PER;
	}

	static void flashProgramDoubleWord(uint32_t addr, uint64_t data) {
		flashWaitBusy();
		FLASH->CR |= FLASH_CR_PG;

		// STM32G4 flash programming in 64-bit words
		*reinterpret_cast<volatile uint32_t *>(addr) = static_cast<uint32_t>(data);
		*reinterpret_cast<volatile uint32_t *>(addr + 4) = static_cast<uint32_t>(data >> 32);

		flashWaitBusy();
		FLASH->CR &= ~FLASH_CR_PG;
	}

	void save() {
		uint32_t primask = __get_PRIMASK();
		__disable_irq();

		flashUnlock();
		flashErasePage(SETTINGS_PAGE);

		// Write in 64-bit words
		const auto *src = reinterpret_cast<const uint64_t *>(&ramCopy);
		uint32_t addr = SETTINGS_ADDR;
		for (uint32_t i = 0; i < sizeof(SettingsData); i += 8) {
			uint64_t word = 0xFFFFFFFFFFFFFFFF;
			uint32_t remaining = sizeof(SettingsData) - i;
			uint32_t copyLen = remaining < 8 ? remaining : 8;
			std::memcpy(&word, reinterpret_cast<const uint8_t *>(src) + i, copyLen);
			flashProgramDoubleWord(addr + i, word);
		}

		// Clear the error flags
		FLASH->SR = FLASH->SR;

		flashLock();

		if (!primask) {
			__enable_irq();
		}
		Debug::log(Debug::Level::INFO, "set", "saved, remote %s", ramCopy.remoteAddr[0] ? ramCopy.remoteAddr : "-");
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
