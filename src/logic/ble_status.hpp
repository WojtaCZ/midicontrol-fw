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
}
