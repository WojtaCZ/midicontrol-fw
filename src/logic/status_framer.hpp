#pragma once
// Splits the module's status messages (%...%) out of the received byte stream. Hardware-independent: compiled into the
// firmware and into the host tests (test/host/test_status_framer.cpp).
//
// It must resynchronise by itself. A single '%' that is lost (USART overrun, a status cut off by a module reset) or one
// too many leaves a plain "inside a status" flag set for good: "CMD> " after "$$$" is then swallowed as status text,
// every "$$$" times out, and an RST restart does not help because "%REBOOT%" only flips the flag back and forth (bench
// 2026-11-13: base "RN4870 init failed again" every 60 s until an MCU reset; the remote had the same bug, fixed
// 2026-11-12 with its own framer). A status arrives in one UART burst of printable ASCII, so a control byte (FRAME_START
// 0x01 begins every frame, command replies end in \r\n) or a pause of more than GAP_MS inside a status ends it, and the
// byte goes back to the caller. Unlike the remote's framer, over-long text is truncated, not abandoned: scan results
// ("F") are longer than the buffer and still count as one status.
#include <cstddef>
#include <cstdint>

namespace statusframe {

	constexpr char DELIMITER = '%';

	enum class Result : uint8_t {
		NOT_STATUS, // the byte is not status text: the caller handles it (frame, command reply)
		CONSUMED,   // part of a status (a delimiter or text)
		COMPLETE    // the closing '%': text() holds the status
	};

	class Framer {
	public:
		static constexpr size_t TEXT_LEN = 96; // including the terminating 0
		static constexpr uint32_t GAP_MS = 50;

		Result feed(uint8_t c, uint32_t nowMs);
		const char *text() const { return text_; }
		bool active() const { return active_; }
		void reset() {
			active_ = false;
			len_ = 0;
		}
		// Statuses abandoned because of a control byte or a pause (a lost or extra '%')
		uint16_t resyncs() const { return resyncs_; }

	private:
		char text_[TEXT_LEN] = {};
		size_t len_ = 0;
		bool active_ = false;
		uint32_t lastMs_ = 0;
		uint16_t resyncs_ = 0;
	};

}
