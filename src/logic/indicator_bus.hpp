#pragma once
// Indicator bus (USART1, 1200 Bd, multi-master, V2 protocol) decisions that do not touch hardware. Host tests in
// test/host/test_indicator_bus.cpp.
#include <cstddef>
#include <cstdint>

namespace indicator {
	inline constexpr size_t	 FRAME_LEN = 9;
	inline constexpr uint8_t FLAG	   = 0xB0;
	// D25: quiet time before the base transmits. 3 byte times at 1200 Bd (10 bits/byte = 8.3 ms) = 25 ms.
	inline constexpr uint32_t QUIET_MS = 25;

	// SYS-BUS-08 (D26): a received frame counts only if it is exactly 9 bytes and starts with the 0xB0 flag.
	bool frameAccepted(const uint8_t* frame, size_t len);

	// SYS-BUS-05 (D25): the base sends only after the bus has been quiet for QUIET_MS and while no frame is coming in.
	class QuietBus {
	  public:
		// Something was heard on the line (a byte, a completed frame) at `nowMs`.
		void onActivity(uint32_t nowMs) {
			_lastMs = nowMs;
			_heard	= true;
		}

		bool maySend(uint32_t nowMs, bool receiving) const {
			if (receiving) return false;
			return !_heard || (nowMs - _lastMs) >= QUIET_MS;
		}

		// Main-loop form: records `receiving` as activity, then decides.
		bool poll(uint32_t nowMs, bool receiving) {
			if (receiving) onActivity(nowMs);
			return maySend(nowMs, receiving);
		}

	  private:
		uint32_t _lastMs = 0;
		bool	 _heard	 = false;
	};
}
