#pragma once
// MIDI 1.0 running status for the DIN OUT (USB -> organ). Hardware independent, unit tested on the host.
//
// A channel message (80..EF) whose status byte equals the last one sent goes out without it: 2 bytes
// (0.64 ms) instead of 3 (0.96 ms) at 31250 baud. Note Off (8n kk vv) is sent as Note On velocity 0
// (9n kk 00, equivalent per MIDI 1.0), so note-off/note-on changes keep the same status too.
//
// Safety rules:
//  - the status is sent again when the last full status is older than kRefreshMs, so a receiver that
//    lost a byte (or was switched on mid-song) recovers within that time;
//  - SysEx and system common (F0..F7) cancel running status (receivers must forget it there too);
//  - real-time bytes (F8..FF) pass through and keep it (allowed by MIDI 1.0).
//
// Build flag: MC_MIDI_RUNNING_STATUS (CMake option MIDI_RUNNING_STATUS, default ON). With 0 every message
// is sent as it came (full status, Note Off as 8n), i.e. the behaviour before 2026-11-22.

#include <cstdint>

namespace running_status {

constexpr uint32_t kRefreshMs = 300;

class Encoder {
public:
	explicit Encoder(bool enabled = true) : _enabled(enabled) {}

	// msg: the MIDI bytes of one USB-MIDI packet (size 1..3, packet[1..size]). Writes the bytes to put on
	// the wire into out (at most 3) and returns how many. nowMs: a free-running millisecond clock.
	uint8_t encode(const uint8_t *msg, uint8_t size, uint32_t nowMs, uint8_t *out) {
		if (size == 0) return 0;
		const uint8_t s = msg[0];

		if (!_enabled || s < 0x80 || s >= 0xF0) {
			// Data byte first (SysEx continuation) or system message: as it came
			if (s < 0xF8) _last = 0; // only real-time keeps the running status
			for (uint8_t i = 0; i < size; ++i) out[i] = msg[i];
			return size;
		}

		// Channel message
		uint8_t status = s;
		uint8_t d1 = size > 1 ? msg[1] : 0;
		uint8_t d2 = size > 2 ? msg[2] : 0;
		if ((status & 0xF0) == 0x80 && size == 3) {
			status = static_cast<uint8_t>(0x90 | (status & 0x0F));
			d2 = 0;
		}

		uint8_t n = 0;
		if (status != _last || static_cast<uint32_t>(nowMs - _lastAtMs) >= kRefreshMs) {
			out[n++] = status;
			_last = status;
			_lastAtMs = nowMs;
		}
		if (size > 1) out[n++] = d1;
		if (size > 2) out[n++] = d2;
		return n;
	}

	// After anything that may have disturbed the line (init, reset, stuck-note cleanup start)
	void reset() { _last = 0; }

	uint8_t lastStatus() const { return _last; }

private:
	bool _enabled;
	uint8_t _last = 0;      // 0 = none, next channel message sends its status
	uint32_t _lastAtMs = 0; // when _last was last sent in full
};

} // namespace running_status
