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

	// A3 (Q2-19, SYS-BUS-07): who wrote the number on the shared bus last. One byte on purpose (RAM budget).
	// NOW_PLAYING_INFO [NPI_CLEAR_IF_OWN] clears only while the writer is Own. Own writes are the base's real
	// changes (NOW_PLAYING_INFO, menu setters), never the periodic resend of the state.
	class WriterTracker {
	  public:
		enum class Writer : uint8_t { None, Own, Foreign };

		// The base put a new number on the bus (it will be, or has been, transmitted).
		void onOwnWrite() { _w = Writer::Own; }

		// An accepted frame arrived from the bus. `ownTx` = it is the echo of our own transmission (or equals what we
		// want on the bus): not a new writer. Everything else is another master.
		void onRxFrame(const uint8_t* /*frame*/, bool ownTx) {
			if (!ownTx) _w = Writer::Foreign;
		}

		bool mayClear() const { return _w == Writer::Own; }

		// The bus was cleared unconditionally (empty NOW_PLAYING_INFO, own clear): nobody owns a number any more.
		void onCleared() { _w = Writer::None; }

		// Conditional clear: true = the caller must clear now (writer becomes None); false = a foreign number (or none)
		// is on the bus, keep it. A skipped clear marks the writer Foreign, unless nothing was written at all.
		bool tryClear() {
			if (_w == Writer::Own) {
				_w = Writer::None;
				return true;
			}
			if (_w != Writer::None) _w = Writer::Foreign;
			return false;
		}

		Writer writer() const { return _w; }

	  private:
		volatile Writer _w = Writer::None; // written by the RX ISR and the main loop
	};
}
