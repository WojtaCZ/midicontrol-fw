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

	// A8: "the base is about to update" announcement to the remote. ENTER_BOOTLOADER starts it; poll() reports when a
	// SIGNAL_INFO [BASE_UPDATING] is due: once at once and once more RESEND_MS later (best effort, no ACK wait; the base
	// resets at 250 ms). While active() the base sends no KEEPALIVE (the remote clears the state on the next one).
	class BootNotice {
	public:
		static constexpr uint32_t RESEND_MS = 100;
		void start(uint32_t now) { _active = true; _sent = 0; _t0 = now; }
		bool active() const { return _active; }
		// true when a frame is due now (call every loop pass; it returns true at most twice per start)
		bool poll(uint32_t now) {
			if (!_active || _sent >= 2) return false;
			if (_sent == 1 && !deadlineReached(now, _t0 + RESEND_MS)) return false;
			++_sent;
			return true;
		}

	private:
		bool	 _active = false;
		uint8_t	 _sent	 = 0;
		uint32_t _t0	 = 0;
	};

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

	// Duplicate detection: the last WINDOW PacketIds processed locally from each node (index = address).
	// A window (not only the last id) also catches a stale retry of an older, already executed command (PM-13);
	// 8 is far below the 256-id wrap.
	class RxDedup {
	public:
		static constexpr uint8_t NODES	= 4;
		static constexpr uint8_t WINDOW = 8;

		// true: a retransmission of a recently processed packet from this source (ACK again, do not execute).
		// false: a new packet, now remembered (the oldest one leaves the window).
		bool isRepeat(uint8_t source, uint8_t packetId);
		// Start afresh for one node (a new PC session, or Q1-8 b: the PC was declared lost)
		void forget(uint8_t source);

	private:
		struct Ring {
			uint8_t ids[WINDOW];
			uint8_t count; // valid entries, up to WINDOW
			uint8_t next;  // slot the next id is written to
		};
		Ring _ring[NODES] = {};
	};

	// C5 (Q2-21b): signals are not ACKed, so the RxDedup never sees them. A retransmitted signal carries the same
	// PacketId as the one just observed; remember only the last id per signal stream (the PC) and skip a repeat.
	class SignalDedup {
	public:
		bool isRepeat(uint8_t packetId) {
			if (_have && _last == packetId) return true;
			_last = packetId;
			_have = true;
			return false;
		}
		void forget() { _have = false; } // PC lost or new PC session: it numbers from 1 again
	private:
		uint8_t _last = 0;
		bool	_have = false;
	};

	// PC session nonce (KEEPALIVE payload [nonce u16 LE], protocol v2): changes when the PC process restarts.
	// 0 means "no nonce" (v1 PC) and never counts.
	class PcSession {
	public:
		// true when the nonce is non-zero and differs from the stored one (the first non-zero nonce counts)
		bool observe(uint16_t nonce);
		void forget() { _nonce = 0; } // the PC was declared lost: the next hello is a new session whatever its nonce
		uint16_t nonce() const { return _nonce; } // 0 = none seen

	private:
		uint16_t _nonce = 0;
	};

	// Which PC session nonce the remote has been told. The base->remote KEEPALIVE carries it; a change must reach
	// the remote before any later frame of the new session (the remote would swallow it as a duplicate).
	class NonceRelay {
	public:
		bool pending(uint16_t current) const { return current != _sent; }
		void sent(uint16_t current) { _sent = current; }

	private:
		uint16_t _sent = 0;
	};

	// Boot id of the remote as the last KEEPALIVE_ACK reported it (protocol v2: [rssi, boot_id u16 LE]; a v1 remote sends
	// 1 byte). Reported in STATUS_RESP so the PC sees a remote reboot shorter than its 5 s poll. Cleared when the
	// remote is declared lost, so a stale id is never reported.
	class RemoteBoot {
	public:
		// KEEPALIVE_ACK payload from the remote; a short payload (v1 remote) changes nothing.
		void onKeepaliveAck(const uint8_t *payload, uint8_t len) {
			if (!payload || len < 3) return;
			_id	   = static_cast<uint16_t>(payload[1] | (payload[2] << 8));
			_valid = true;
		}
		void forget() { _valid = false; }
		bool valid() const { return _valid; }
		uint16_t id() const { return _id; }

	private:
		uint16_t _id	= 0;
		bool	 _valid = false;
	};

	// STATUS_RESP payload (protocol v2, 7 B): [ble, ssr, display, pcAlive, flags, boot_id u16 LE].
	// flags bit0 = remote boot id valid (boot_id is 0 otherwise), bits 1-7 reserved 0. Returns the length (7).
	constexpr uint8_t STATUS_FLAG_REMOTE_BOOT_VALID = 0x01;
	uint8_t buildStatus(uint8_t *payload, bool bleUsable, bool ssrOn, bool displayConnected, bool pcAlive, const RemoteBoot &boot);

	// KEEPALIVE from the PC: a new session nonce makes the duplicate window of the PC forget.
	// Returns true when it did. payload may be empty (v1 PC).
	bool onPcKeepalive(PcSession &session, RxDedup &dedup, const uint8_t *payload, uint8_t len);

	// Reliable packets the base sent itself, waiting for an ACK/NAK
	class RetryQueue {
	public:
		static constexpr uint8_t SIZE = 4;

		struct Entry {
			protocol::Packet pkt;
			uint32_t		 sentAt;
			uint8_t			 retries;
			bool			 used;
			bool			 busy; // PM-6: the last answer was NAK BUSY (fits in the padding, no RAM)
		};

		bool full() const;
		// Remember a packet that was just transmitted; false when the queue is full
		bool add(const protocol::Packet &pkt, uint32_t now);
		// What resolve() closed: the request's type and, for a NAK, the reason (PM-1)
		struct Closed {
			protocol::MsgType	type;
			bool				nak;
			protocol::NakReason reason;
		};
		// ACK/NAK from rx.source for the PacketId in payload[0] closes the matching entry, except NAK BUSY
		// (kept for a retry, PM-6); true when one was closed, then *out (if given) says which and how
		bool resolve(const protocol::Packet &rx, Closed *out = nullptr);
		// Entries without an answer for `timeout` (or answered NAK BUSY): resend(pkt) with the same PacketId,
		// after maxRetries giveUp(pkt, busy), busy = the last answer was NAK BUSY
		template<typename Resend, typename GiveUp>
		void poll(uint32_t now, uint32_t timeout, uint8_t maxRetries, Resend &&resend, GiveUp &&giveUp) {
			for (auto &e : _entries) {
				if (!e.used || (now - e.sentAt) < timeout) continue;
				if (e.retries >= maxRetries) {
					e.used = false;
					giveUp(static_cast<const protocol::Packet &>(e.pkt), e.busy);
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

	// PM-1/UI2-1: why a request the base sent itself failed, as a short OLED text (Czech, no diacritics)
	enum class Failure : uint8_t { Nak, NoAnswer, NotSent };
	const char *failureText(Failure f, protocol::NakReason reason = protocol::NakReason::UNKNOWN_MSG);

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
