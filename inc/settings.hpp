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
		// Version 2: address of the paired controller (12 hex characters + 0, empty = none). Only with it does the base
		// secure the link (B) after %CONNECT%; any other device is disconnected.
		char remoteAddr[13];
	};

	void init();
	void save();

	bool ledMasterEnable();
	void setLedMasterEnable(bool value);

	bool midiRoleIdentifier();
	void setMidiRoleIdentifier(bool value);

	// Address of the paired controller ("" = none)
	const char *remoteAddr();
	void setRemoteAddr(const char *addr);

} // namespace settings

#endif
