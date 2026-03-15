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
	};

	void init();
	void save();

	bool ledMasterEnable();
	void setLedMasterEnable(bool value);

	bool midiRoleIdentifier();
	void setMidiRoleIdentifier(bool value);

} // namespace settings

#endif
