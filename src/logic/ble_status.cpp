#include "ble_status.hpp"

#include <cstring>

namespace blestatus {

	bool isWord(const char *s, const char *word) {
		size_t len = strlen(word);
		return strncmp(s, word, len) == 0 && (s[len] == '\0' || s[len] == ',');
	}

	bool parseConnect(const char *s, int &type, char *mac, size_t macSize) {
		static constexpr char PREFIX[] = "CONNECT,";
		static constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1;
		if (macSize == 0 || strncmp(s, PREFIX, PREFIX_LEN) != 0) return false;
		const char *p = s + PREFIX_LEN;
		if (p[0] < '0' || p[0] > '9' || p[1] != ',') return false;
		const char *addr = p + 2;
		size_t len = strcspn(addr, ",");
		if (len == 0) return false;
		if (len > macSize - 1) len = macSize - 1;
		memcpy(mac, addr, len);
		mac[len] = '\0';
		type = p[0] - '0';
		return true;
	}

	Health health(bool starting, bool configured, bool resetOk, unsigned long rxBytes) {
		if (configured) return resetOk ? Health::OK : Health::OK_NO_RESET;
		if (starting) return Health::STARTING;
		return rxBytes > 0 ? Health::NO_REPLY : Health::SILENT;
	}

	const char *healthText(Health h) {
		switch (h) {
			case Health::STARTING:    return "Modul: start...";
			case Health::OK:          return "Modul: OK";
			case Health::OK_NO_RESET: return "Modul: OK, bez RST";
			case Health::NO_REPLY:    return "Modul: neodpovida";
			case Health::SILENT:      return "Modul: mlci";
		}
		return "Modul: ?";
	}

}
