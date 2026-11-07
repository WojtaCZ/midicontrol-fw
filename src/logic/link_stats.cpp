#include "link_stats.hpp"

#include <cstring>

namespace linkstats {

	namespace {
		bool hexDigit(char c, unsigned &v) {
			if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
			else if (c >= 'A' && c <= 'F') v = static_cast<unsigned>(c - 'A' + 10);
			else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
			else return false;
			return true;
		}

		// Hex field up to the next comma or the end; false when empty, too long (> 4 digits) or not hex
		bool hexField(const char *&p, unsigned &out) {
			size_t n = 0;
			unsigned v = 0, d = 0;
			while (p[n] && p[n] != ',') {
				if (!hexDigit(p[n], d) || n >= 4) return false;
				v = v * 16 + d;
				n++;
			}
			if (n == 0) return false;
			out = v;
			p += n;
			if (*p == ',') p++;
			return true;
		}

		uint16_t sat(uint16_t v) { return v == 0xFFFF ? v : static_cast<uint16_t>(v + 1); }
		int8_t clampDbm(int v) { return static_cast<int8_t>(v < -127 ? -127 : (v > 20 ? 20 : v)); }
	}

	bool parseConnParam(const char *s, ConnParams &out) {
		static constexpr char PREFIX[] = "CONN_PARAM,";
		if (strncmp(s, PREFIX, sizeof(PREFIX) - 1) != 0) return false;
		const char *p = s + sizeof(PREFIX) - 1;
		unsigned i, l, t;
		if (!hexField(p, i) || !hexField(p, l) || !hexField(p, t)) return false;
		out.interval = static_cast<uint16_t>(i);
		out.latency = static_cast<uint16_t>(l);
		out.timeout = static_cast<uint16_t>(t);
		return true;
	}

	bool parsePhyUpdated(const char *s, uint8_t &tx, uint8_t &rx) {
		static constexpr char PREFIX[] = "PHY_UPDATED,";
		if (strncmp(s, PREFIX, sizeof(PREFIX) - 1) != 0) return false;
		const char *p = s + sizeof(PREFIX) - 1;
		unsigned f[3];
		size_t n = 0;
		while (*p && n < 3) {
			if (!hexField(p, f[n])) return false;
			n++;
		}
		if (*p || n < 2) return false;
		tx = static_cast<uint8_t>(f[n - 2]);
		rx = static_cast<uint8_t>(f[n - 1]);
		return true;
	}

	bool connectHandle(const char *s, char *out, size_t cap) {
		static constexpr char PREFIX[] = "CONNECT,";
		if (cap == 0 || strncmp(s, PREFIX, sizeof(PREFIX) - 1) != 0) return false;
		const char *p = s + sizeof(PREFIX) - 1;
		for (int field = 0; field < 2; ++field) { // skip <type> and <MAC>
			p = strchr(p, ',');
			if (!p) return false;
			p++;
		}
		size_t n = strcspn(p, ",");
		unsigned d;
		if (n == 0 || n >= cap || n > 4) return false;
		for (size_t i = 0; i < n; ++i) {
			if (!hexDigit(p[i], d)) return false;
			out[i] = p[i];
		}
		out[n] = '\0';
		return true;
	}

	bool parseRssi(const char *line, int8_t &dbm) {
		bool negative = line[0] == '-';
		const char *p = negative ? line + 1 : line;
		size_t len = strlen(p);
		if (len == 0 || len > 2) return false;
		unsigned v = 0, d = 0;
		for (size_t i = 0; i < len; ++i) {
			if (!hexDigit(p[i], d)) return false;
			v = v * 16 + d;
		}
		dbm = negative ? clampDbm(-static_cast<int>(v)) : clampDbm(static_cast<int8_t>(v));
		return true;
	}

	void Stats::onConnect() {
		rssi = RSSI_UNKNOWN;
		rssiEver = false;
		peerRssi = RSSI_UNKNOWN;
		peerRssiEver = false;
		params = {};
		phyTx = phyRx = 0;
	}

	void Stats::onLost(bool isErrConn, uint32_t nowMs) {
		c.losses = sat(c.losses);
		if (isErrConn) c.errConn = sat(c.errConn);
		lossAt = nowMs;
		lossPending = true;
		rssi = RSSI_UNKNOWN;
		rssiEver = false;
	}

	uint32_t Stats::onUnexpectedReboot(uint32_t nowMs) {
		c.reboots = sat(c.reboots);
		uint32_t since = rebootEver ? nowMs - lastRebootAt : 0;
		rebootEver = true;
		lastRebootAt = nowMs;
		return since;
	}

	void Stats::onStreamOpen(uint32_t nowMs) {
		if (!lossPending) return;
		lossPending = false;
		uint32_t d = nowMs - lossAt;
		c.lastReconnectMs = d > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(d);
	}

	void Stats::onSecureRetry() { c.secureRetries = sat(c.secureRetries); }

	void Stats::setRssi(int8_t dbm, uint32_t nowMs) {
		rssi = dbm;
		rssiAt = nowMs;
		rssiEver = true;
	}

	void Stats::setPeerRssi(int8_t dbm, uint32_t nowMs) {
		peerRssi = dbm;
		peerRssiAt = nowMs;
		peerRssiEver = true;
	}

	bool Stats::onStatus(const char *s) {
		ConnParams cp;
		uint8_t tx, rx;
		if (parseConnParam(s, cp)) {
			params = cp;
			return true;
		}
		if (parsePhyUpdated(s, tx, rx)) {
			phyTx = tx;
			phyRx = rx;
			return true;
		}
		return false;
	}

	int8_t Stats::rssiNow(uint32_t nowMs) const { return (rssiEver && nowMs - rssiAt <= RSSI_STALE_MS) ? rssi : RSSI_UNKNOWN; }
	int8_t Stats::peerRssiNow(uint32_t nowMs) const { return (peerRssiEver && nowMs - peerRssiAt <= RSSI_STALE_MS) ? peerRssi : RSSI_UNKNOWN; }

	InfoFields Stats::infoFields(uint32_t nowMs, bool live) const {
		auto sat8 = [](uint16_t v) { return static_cast<uint8_t>(v > 255 ? 255 : v); };
		InfoFields f{};
		f.rssi[0] = static_cast<uint8_t>(live ? rssiNow(nowMs) : RSSI_UNKNOWN);
		f.rssi[1] = static_cast<uint8_t>(live ? peerRssiNow(nowMs) : RSSI_UNKNOWN);
		f.params[0] = static_cast<uint8_t>(params.interval);
		f.params[1] = static_cast<uint8_t>(params.interval >> 8);
		f.params[2] = static_cast<uint8_t>(params.timeout);
		f.params[3] = static_cast<uint8_t>(params.timeout >> 8);
		f.params[4] = sat8(params.latency);
		f.params[5] = static_cast<uint8_t>(((phyTx & 0x0F) << 4) | (phyRx & 0x0F));
		f.counters[0] = sat8(c.losses);
		f.counters[1] = sat8(c.errConn);
		f.counters[2] = sat8(c.reboots);
		f.counters[3] = sat8(c.secureRetries);
		f.counters[4] = static_cast<uint8_t>(c.lastReconnectMs);
		f.counters[5] = static_cast<uint8_t>(c.lastReconnectMs >> 8);
		return f;
	}
}
