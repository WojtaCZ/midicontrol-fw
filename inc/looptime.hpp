#ifndef LOOPTIME_HPP
#define LOOPTIME_HPP

#include <cstdint>
#include <stm32g431xx.h>
#include <core_cm4.h>

// Main-loop timing instrumentation (bench only, read by probe like mcStaleKicks; no wire protocol, no log output).
// All times are DWT cycle counts at 144 MHz (1 us = 144 cycles). Cost: about 12 CYCCNT reads and compares per loop pass.
//
//   pass       one pass of the main loop WITHOUT the time spent in blocking DIN sends (midi::send ~0.96 ms per 3-byte message)
//   passRaw    the same including the DIN send time (max only)
//   sect[i]    time spent in section i of the pass (max and count over 1 ms); includes interrupts that ran meanwhile
//   lat        USB MIDI packet to DIN byte start: from the first USB interrupt since the previous pass until the first byte of
//              a message is written to USART3 (an upper bound: the interrupt may be for another endpoint, e.g. CDC).
//              latFirst = only the first message of a drain batch (pure processing delay), lat = every message (a chord's
//              4th message includes the 3 messages in front of it on the wire).
// The probe writes 1 to `mcLoopReset` to clear everything (the loop does it on its next pass).
namespace looptime {

	enum Section : uint8_t {
		S_TUD, S_USB_DIN, S_MIDI, S_DISPLAY, S_BLE, S_ROUTER, S_DEBUG, S_SCHED, S_BLEPROC, S_REST, S_COUNT
	};

	static constexpr unsigned HIST = 8;
	// Upper edges in us of histogram buckets 0..6; bucket 7 = above the last edge
	static constexpr uint32_t EDGE_US[HIST - 1] = {50, 100, 250, 500, 1000, 2000, 5000};

	struct Stats {
		uint32_t version;            // 1
		uint32_t passes;
		uint32_t maxPass;            // cycles, without DIN send time
		uint32_t maxPassRaw;         // cycles, with DIN send time
		uint32_t histPass[HIST];
		uint32_t sectMax[S_COUNT];   // cycles
		uint32_t sectOver1ms[S_COUNT];
		uint32_t msgs;               // DIN messages sent
		uint32_t latMax, latFirstMax;
		uint32_t latHist[HIST], latFirstHist[HIST];
		uint32_t sendMax;            // longest single midi::send (cycles)
		uint32_t cyclesPerUs;        // 144, lets the reader convert without assumptions
	};

	extern volatile Stats stats;
	extern volatile uint32_t reset;
	extern volatile uint32_t usbIrqStamp; // first USB IRQ since the last pass (0 = none)

	inline uint32_t now() { return DWT->CYCCNT; }

	inline void enable() {
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CYCCNT = 0;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
		stats.version = 1;
		stats.cyclesPerUs = 144;
	}

	inline unsigned bucket(uint32_t cycles) {
		for (unsigned i = 0; i < HIST - 1; ++i) {
			if (cycles < EDGE_US[i] * 144u) return i;
		}
		return HIST - 1;
	}

	inline void clear() {
		volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(&stats);
		for (unsigned i = 0; i < sizeof(Stats) / 4; ++i) p[i] = 0;
		stats.version = 1;
		stats.cyclesPerUs = 144;
	}

	// From the USB interrupt handlers: remember the first one since the last pass (stamp 0 is mapped to 1)
	inline void onUsbIrq() {
		if (usbIrqStamp == 0) usbIrqStamp = now() | 1u;
	}

	struct Pass {
		uint32_t start = 0, last = 0, sendCycles = 0, irqStamp = 0;
		bool batchFirst = true;
		bool running = false;

		// Top of the loop: closes the previous pass, opens a new one
		void begin() {
			const uint32_t t = now();
			if (reset != 0) {
				reset = 0;
				clear();
				running = false;
			}
			if (running) {
				const uint32_t raw = t - start;
				const uint32_t net = raw - sendCycles;
				stats.passes = stats.passes + 1;
				if (net > stats.maxPass) stats.maxPass = net;
				if (raw > stats.maxPassRaw) stats.maxPassRaw = raw;
				const unsigned b = bucket(net);
				stats.histPass[b] = stats.histPass[b] + 1;
			}
			running = true;
			start = last = t;
			sendCycles = 0;
			irqStamp = usbIrqStamp;
			usbIrqStamp = 0;
			batchFirst = true;
		}

		// End of section s
		void lap(Section s) {
			const uint32_t t = now();
			const uint32_t d = t - last;
			last = t;
			if (d > stats.sectMax[s]) stats.sectMax[s] = d;
			if (d > 144000u) stats.sectOver1ms[s] = stats.sectOver1ms[s] + 1;
		}

		// Just before midi::send of one message; returns the stamp for sent()
		uint32_t beforeSend() {
			const uint32_t t = now();
			if (irqStamp != 0) {
				const uint32_t l = t - irqStamp;
				if (l > stats.latMax) stats.latMax = l;
				const unsigned b = bucket(l);
				stats.latHist[b] = stats.latHist[b] + 1;
				if (batchFirst) {
					if (l > stats.latFirstMax) stats.latFirstMax = l;
					stats.latFirstHist[b] = stats.latFirstHist[b] + 1;
				}
			}
			batchFirst = false;
			return t;
		}

		void afterSend(uint32_t t0) {
			const uint32_t d = now() - t0;
			sendCycles += d;
			stats.msgs = stats.msgs + 1;
			if (d > stats.sendMax) stats.sendMax = d;
		}
	};

} // namespace looptime

#endif
