#pragma once
// Hardware-independent parts of the base router (src/router.cpp), host-tested in test/host/test_router_core.cpp:
// peer presence from received packets, duplicate detection of reliable packets and the retry queue of the
// base's own reliable packets. Time is a free-running uint32 counter supplied by the caller.

#include <cstdint>

#include "protocol/types.hpp"

namespace routercore {

	// Milliseconds since boot from the SysTick counter (tick period in µs)
	uint32_t msFromTicks(uint32_t ticks, uint32_t tickUs);

	// Has the deadline been reached? (both on the same free-running clock; wrap-safe while less than 2^31 apart)
	bool deadlineReached(uint32_t now, uint32_t deadline);

	// Presence of a peer (the PC on USB-CDC): alive from a received packet, lost after `loss` without one.
	// Lost is a latch (BF-1): only a received packet makes the peer alive again, never the clock wrapping.
	class PeerPresence {
	public:
		explicit constexpr PeerPresence(uint32_t loss) : _loss(loss) {}

		void onRx(uint32_t now);	 // a valid packet from the peer was received
		void update(uint32_t now);	 // called on every main-loop pass
		bool alive() const { return _alive; }
		bool everSeen() const { return _everSeen; }
		uint32_t lastRx() const { return _lastRx; }

	private:
		// Layout read by tools/basetest (test_lifecycle.py pc_alive()): _alive at offset 9
		uint32_t _loss;
		uint32_t _lastRx   = 0;
		bool	 _everSeen = false;
		bool	 _alive	   = false;
	};

	// Duplicate detection: the last PacketId processed locally from each node (index = address)
	class RxDedup {
	public:
		static constexpr uint8_t NODES = 4;

		// true: a retransmission of the last packet from this source (ACK again, do not execute).
		// false: a new packet, now remembered.
		bool isRepeat(uint8_t source, uint8_t packetId);
		// Start afresh for one node (Q1-8 b: the PC was declared lost, its next process restarts the PacketIds)
		void forget(uint8_t source);

	private:
		struct LastRx {
			uint8_t packetId;
			bool	valid;
		};
		LastRx _last[NODES] = {};
	};

	// Reliable packets the base sent itself, waiting for an ACK/NAK
	class RetryQueue {
	public:
		static constexpr uint8_t SIZE = 4;

		struct Entry {
			protocol::Packet pkt;
			uint32_t		 sentAt;
			uint8_t			 retries;
			bool			 used;
		};

		bool full() const;
		// Remember a packet that was just transmitted; false when the queue is full
		bool add(const protocol::Packet &pkt, uint32_t now);
		// ACK/NAK from rx.source for the PacketId in payload[0] closes the matching entry
		void resolve(const protocol::Packet &rx);
		// Entries without an answer for `timeout`: resend(pkt) with the same PacketId, after maxRetries giveUp(pkt)
		template<typename Resend, typename GiveUp>
		void poll(uint32_t now, uint32_t timeout, uint8_t maxRetries, Resend &&resend, GiveUp &&giveUp) {
			for (auto &e : _entries) {
				if (!e.used || (now - e.sentAt) < timeout) continue;
				if (e.retries >= maxRetries) {
					e.used = false;
					giveUp(static_cast<const protocol::Packet &>(e.pkt));
					continue;
				}
				resend(static_cast<const protocol::Packet &>(e.pkt));
				e.retries++;
				e.sentAt = now;
			}
		}

	private:
		Entry _entries[SIZE] = {};
	};

	// Q1-1 c: base backstop for organ power. SSR on and no valid PC packet for 2 h (PC lost, absent or
	// never seen) -> the router switches the SSR off and sends All Notes Off. A PC packet restarts the
	// 2 h, so does the SSR going off -> on. Times are router-clock ms, compared modulo 2^32.
	class SsrBackstop {
	public:
		static constexpr uint32_t TIMEOUT_MS = 2u * 60u * 60u * 1000u;
		void onPcPacket(uint32_t now) { _since = now; }
		// Call every pass with the current SSR state; true once when the 2 h ran out.
		bool poll(uint32_t now, bool ssrOn) {
			if (!ssrOn) {
				_wasOn = false;
				return false;
			}
			if (!_wasOn) { // off -> on (menu, PC SET_CURRENT): a new 2 h
				_wasOn = true;
				_since = now;
				return false;
			}
			if (now - _since < TIMEOUT_MS) return false;
			_wasOn = false; // the caller switches the SSR off; the next on starts a new 2 h
			return true;
		}
	private:
		uint32_t _since = 0;
		bool	 _wasOn = false;
	};
} // namespace routercore
