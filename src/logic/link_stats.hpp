#pragma once
// Link quality bookkeeping of the BLE module (RNBD350): RSSI, live connection parameters, PHY and link counters.
// Hardware independent, identical in the base and the remote firmware; host tests in test/host/test_link_stats.cpp.
// Exposed in INFO_RESP (protocol::info::Tag::LINK_*) and the Bluetooth info page.
#include <cstddef>
#include <cstdint>

namespace linkstats {

	static constexpr int8_t RSSI_UNKNOWN = -128;
	// A reading older than this is reported as unknown (the poll runs about every 10 s)
	static constexpr uint32_t RSSI_STALE_MS = 25000;

	// Raw module units: interval 1.25 ms, latency in connection events, timeout 10 ms
	struct ConnParams {
		uint16_t interval = 0, latency = 0, timeout = 0;
	};

	// "CONN_PARAM,<interval>,<latency>,<timeout>[,<handle>]": all fields are HEX (RNBD350 bench 2026-11-04: CONN_PARAM,0010,0000,0048,0072)
	bool parseConnParam(const char *s, ConnParams &out);
	// "PHY_UPDATED,<handle>,<tx>,<rx>" (1 = 1M, 2 = 2M, 3 = Coded); a handle-less "PHY_UPDATED,<tx>,<rx>" is accepted too
	bool parsePhyUpdated(const char *s, uint8_t &tx, uint8_t &rx);
	// Connection handle of "CONNECT,<type>,<MAC>,<handle>" (RNBD350) as the module wrote it ("0072"); false for the handle-less RN4870 form
	bool connectHandle(const char *s, char *out, size_t cap);
	// One line of the reply to M / QRSS: "-35" or "-5A" (sign + 1..2 hex digits, like every RN number) or a bare two's complement byte "B5"
	bool parseRssi(const char *line, int8_t &dbm);

	struct Counters {
		uint16_t losses = 0;          // link lost: %DISCONNECT% or %ERR_CONN% while connected
		uint16_t errConn = 0;         // %ERR_CONN% while connected (supervision timeout / vanished peer)
		uint16_t reboots = 0;         // %REBOOT% the module reported without our R,1 / RST (brown-out, module reset)
		uint16_t secureRetries = 0;   // B / K,1 retries of the secure watchdog
		uint16_t lastReconnectMs = 0; // from the last loss to the next %STREAM_OPEN% (saturating)
	};

	// The three INFO_RESP link tags (protocol::info::Tag::LINK_RSSI / LINK_PARAMS / LINK_COUNTERS), 20 bytes with the TLV headers:
	// the remote's payload is nearly full, so the values are packed (RSSI own + peer, latency / PHY as bytes, 8-bit counters)
	struct InfoFields {
		uint8_t rssi[2];     // i8 own, i8 peer; 0x80 unknown
		uint8_t params[6];   // u16 interval, u16 timeout (little-endian), u8 latency, u8 tx << 4 | rx PHY
		uint8_t counters[6]; // u8 losses, errConn, reboots, secureRetries; u16 last reconnect ms
	};

	struct Stats {
		Counters c;
		int8_t rssi = RSSI_UNKNOWN;
		uint32_t rssiAt = 0;
		bool rssiEver = false;
		ConnParams params;
		uint8_t phyTx = 0, phyRx = 0;
		int8_t peerRssi = RSSI_UNKNOWN; // what the other end measured, from its KEEPALIVE / KEEPALIVE_ACK
		uint32_t peerRssiAt = 0;
		bool peerRssiEver = false;
		uint32_t lossAt = 0;
		bool lossPending = false;
		uint32_t lastRebootAt = 0;
		bool rebootEver = false;

		// A new link: what was measured on the previous one no longer applies (counters stay)
		void onConnect();
		void onLost(bool errConn, uint32_t nowMs);
		// Returns ms since the previous unexpected reboot (0 = first one)
		uint32_t onUnexpectedReboot(uint32_t nowMs);
		void onStreamOpen(uint32_t nowMs);
		void onSecureRetry();
		void setRssi(int8_t dbm, uint32_t nowMs);
		void setPeerRssi(int8_t dbm, uint32_t nowMs);
		// Handles %CONN_PARAM% and %PHY_UPDATED% (text without the delimiters); true when it consumed the status
		bool onStatus(const char *s);
		int8_t rssiNow(uint32_t nowMs) const;
		int8_t peerRssiNow(uint32_t nowMs) const;
		// live = a connection exists (otherwise the RSSI values read unknown)
		InfoFields infoFields(uint32_t nowMs, bool live) const;
	};
}
