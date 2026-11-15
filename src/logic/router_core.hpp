#pragma once
// Hardware-independent parts of the base router (src/router.cpp), host-tested in test/host/test_router_core.cpp:
// peer presence from received packets, duplicate detection of reliable packets and the retry queue of the
// base's own reliable packets. Time is a free-running uint32 counter supplied by the caller.

#include <cstdint>

#include "protocol/types.hpp"

namespace routercore {

	// Milliseconds since boot from the SysTick counter (tick period in µs)
	uint32_t msFromTicks(uint32_t ticks, uint32_t tickUs);

	// Has the deadline been reached? (both on the same free-running clock)
	bool deadlineReached(uint32_t now, uint32_t deadline);

	// Presence of a peer (the PC on USB-CDC): alive while packets keep arriving, lost after `loss` without one.
	class PeerPresence {
	public:
		explicit constexpr PeerPresence(uint32_t loss) : _loss(loss) {}

		void onRx(uint32_t now);	 // a valid packet from the peer was received
		void update(uint32_t now);	 // called on every main-loop pass
		bool alive() const;
		bool everSeen() const { return _everSeen; }
		uint32_t lastRx() const { return _lastRx; }

	private:
		uint32_t _loss;
		uint32_t _lastRx = 0;
		uint32_t _now	 = 0;
		bool	 _everSeen = false;
	};

	// Duplicate detection: the last PacketId processed locally from each node (index = address)
	class RxDedup {
	public:
		static constexpr uint8_t NODES = 4;

		// true: a retransmission of the last packet from this source (ACK again, do not execute).
		// false: a new packet, now remembered.
		bool isRepeat(uint8_t source, uint8_t packetId);

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

} // namespace routercore
