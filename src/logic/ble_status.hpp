#pragma once
// Status text (%...%, without the delimiters) of the RN4870 and of the RNBD350 that replaces it.
// The RNBD350 appends a connection handle to some events the RN4870 sends bare (bench 2026-11-04):
//   %CONNECT,0,10B1DF04F934,0072%  %DISCONNECT,0072%  %CONN_PARAM,0010,0000,0048,0072%
// Hardware independent; host tests in test/host/test_ble_status.cpp.
#include <cstddef>

namespace blestatus {
	// s is "word" or "word,<fields>"
	bool isWord(const char *s, const char *word);

	// "CONNECT,<address type>,<MAC>[,<handle>]": type 0..9, MAC up to the next comma (truncated to macSize - 1).
	// false when the text is not a CONNECT event with an address type and a non-empty MAC.
	bool parseConnect(const char *s, int &type, char *mac, size_t macSize);

	// SYS-LINK-07: the remote link is usable only when connected, the stream is open and the peer accepted (secured with
	// the bonded address). Used for GET_STATUS bleConnected and for BUSY vs UNREACHABLE.
	constexpr bool linkUsable(bool connected, bool streamOpen, bool peerAccepted) {
		return connected && streamOpen && peerAccepted;
	}

	// Module self-test for the Bluetooth info page, so a re-soldered module can be checked without a probe.
	//   OK           configured, %REBOOT% came after the RST pulse
	//   OK_NO_RESET  commands work, but no %REBOOT% after the RST pulse: RST -> NMCLR
	//   NO_REPLY     the module sent something, but the configuration failed: MCU TX -> module RX
	//   SILENT       nothing ever received from the module: module TX -> MCU RX or its power (or RX and RST both open)
	enum class Health : unsigned char { STARTING, OK, OK_NO_RESET, NO_REPLY, SILENT };
	// starting: configuration or restart still running; configured: module up; resetOk: %REBOOT% after the last RST pulse;
	// rxBytes: bytes received from the module since power-on
	Health health(bool starting, bool configured, bool resetOk, unsigned long rxBytes);
	// One display line (at most 18 characters, 7x10 font)
	const char *healthText(Health h);

	// ── Scan results (F) ──
	// A field of a status line: points into the parsed text, not NUL-terminated
	struct ScanField {
		const char *p = "";
		size_t len = 0;
	};
	struct ScanResult {
		ScanField addr{};   // 12 hex digits
		int type = 0;     // address type, 0 public / 1 random (what C,<type>,<addr> takes)
		bool connectable = false;
		ScanField name{};   // connectable only, may be empty
		ScanField uuids{};  // connectable only, may be empty
		ScanField data{};   // non-connectable only: advertising data after "Brcst:"
		int rssi = 0;     // dBm; informational only, nothing decides on it
	};
	// One advertisement reported by F. The fields are hex; the RSSI is a two's complement byte on the RN4870 and
	// signed on the RNBD350 (sign and two hex digits, assumed hex like every other RN number; bench 2026-11-05).
	//   RN4870  connectable      <addr>,<0|1>,<name>,<uuids>,<rssi>                 D88039FE5996,0,MIDIControlRemote,,F5
	//           non-connectable  <addr>,<0|1>,<rssi>,Brcst:<data>                   CAD46E49E827,1,AD,Brcst:07FF4C0012020001
	//   RNBD350 connectable      <addr>,<00|01>,<name>,<uuids>,<rssi>,<x>,<y>       40843204625F,00,MIDIControlRemote,FEDA,-35,01,00
	//           non-connectable  <addr>,<00|01>,<rssi>,<x>,<y>,Brcst:<data>         8C79F5D3DE29,00,-59,01,00,Brcst:1BFF7500...
	// The RNBD350 reports the advertisement and the scan response separately (the same address twice).
	// false for any other text (status events etc.).
	bool parseScan(const char *s, ScanResult &out);
}
