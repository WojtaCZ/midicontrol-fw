#ifndef SETTINGS_H
#define SETTINGS_H

#include <cstdint>

namespace settings {

	struct SettingsData {
		uint32_t magic;
		uint8_t version;
		bool ledMasterEnable;
		bool midiRoleId;
		uint8_t _pad;
		// Verze 2: adresa spárovaného ovladače (12 hex znaků + 0, prázdná = žádný). Jen s ní base po
		// %CONNECT% zabezpečí spojení (B); jiné zařízení odpojí.
		char remoteAddr[13];
	};

	void init();
	void save();

	bool ledMasterEnable();
	void setLedMasterEnable(bool value);

	bool midiRoleIdentifier();
	void setMidiRoleIdentifier(bool value);

	// Adresa spárovaného ovladače ("" = žádný)
	const char *remoteAddr();
	void setRemoteAddr(const char *addr);

} // namespace settings

#endif
