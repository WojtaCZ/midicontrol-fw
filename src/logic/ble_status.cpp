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


	namespace {
		bool isHex(const char *p, size_t len) {
			if (len == 0) return false;
			for (size_t i = 0; i < len; ++i) {
				char c = p[i];
				if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
			}
			return true;
		}

		int hexValue(const char *p, size_t len) {
			int v = 0;
			for (size_t i = 0; i < len; ++i) {
				char c = p[i];
				v = v * 16 + ((c <= '9') ? c - '0' : ((c | 0x20) - 'a' + 10));
			}
			return v;
		}

		// "F5" (RN4870, two's complement byte) or "-35" (RNBD350)
		bool parseRssi(ScanField f, int &rssi) {
			bool negative = f.len > 0 && f.p[0] == '-';
			const char *p = negative ? f.p + 1 : f.p;
			size_t len = negative ? f.len - 1 : f.len;
			if (len == 0 || len > 2 || !isHex(p, len)) return false;
			int v = hexValue(p, len);
			rssi = negative ? -v : static_cast<signed char>(v);
			return true;
		}
	}

	bool parseScan(const char *s, ScanResult &out) {
		constexpr size_t MAX_FIELDS = 8;
		ScanField f[MAX_FIELDS];
		size_t n = 0;
		const char *p = s;
		while (true) {
			if (n == MAX_FIELDS) return false;
			const char *comma = strchr(p, ',');
			f[n].p = p;
			f[n].len = comma ? static_cast<size_t>(comma - p) : strlen(p);
			n++;
			if (!comma) break;
			p = comma + 1;
		}
		if (n < 4) return false;
		if (f[0].len != 12 || !isHex(f[0].p, 12)) return false;
		if (f[1].len < 1 || f[1].len > 2 || !isHex(f[1].p, f[1].len)) return false;
		int type = hexValue(f[1].p, f[1].len);
		if (type > 1) return false;

		ScanResult r;
		r.addr = f[0];
		r.type = type;
		const ScanField &last = f[n - 1];
		if (last.len >= 6 && strncmp(last.p, "Brcst:", 6) == 0) {
			if (n != 4 && n != 6) return false;
			if (!parseRssi(f[2], r.rssi)) return false;
			r.data = {last.p + 6, last.len - 6};
		} else {
			if (n != 5 && n != 7) return false;
			if (!parseRssi(f[4], r.rssi)) return false;
			r.connectable = true;
			r.name = f[2];
			r.uuids = f[3];
		}
		out = r;
		return true;
	}
}

