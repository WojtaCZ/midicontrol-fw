#include "router_core.hpp"

namespace routercore {

	uint32_t msFromTicks(uint32_t ticks, uint32_t tickUs) {
		// 64-bit product: getDuration() / 1_ms wrapped at 2^32 µs (71.58 min); this wraps at 2^32 ms (BF-1)
		return static_cast<uint32_t>((static_cast<uint64_t>(ticks) * tickUs) / 1000u);
	}

	bool deadlineReached(uint32_t now, uint32_t deadline) {
		return static_cast<int32_t>(now - deadline) >= 0;
	}

	void PeerPresence::onRx(uint32_t now) {
		_lastRx	  = now;
		_everSeen = true;
		_alive	  = true;
	}

	void PeerPresence::update(uint32_t now) {
		if (_alive && (now - _lastRx) >= _loss) _alive = false;
	}

	bool RxDedup::isRepeat(uint8_t source, uint8_t packetId) {
		if (source >= NODES) return false;
		Ring &r = _ring[source];
		for (uint8_t i = 0; i < r.count; ++i) {
			if (r.ids[i] == packetId) return true;
		}
		r.ids[r.next] = packetId;
		r.next		  = static_cast<uint8_t>((r.next + 1) % WINDOW);
		if (r.count < WINDOW) ++r.count;
		return false;
	}

	void RxDedup::forget(uint8_t source) {
		if (source < NODES) _ring[source] = {};
	}

	bool PcSession::observe(uint16_t nonce) {
		if (nonce == 0 || nonce == _nonce) return false;
		_nonce = nonce;
		return true;
	}

	uint8_t buildStatus(uint8_t *payload, bool bleUsable, bool ssrOn, bool displayConnected, bool pcAlive, const RemoteBoot &boot) {
		const uint16_t id = boot.valid() ? boot.id() : 0;
		payload[0]		  = static_cast<uint8_t>(bleUsable);
		payload[1]		  = static_cast<uint8_t>(ssrOn);
		payload[2]		  = static_cast<uint8_t>(displayConnected);
		payload[3]		  = static_cast<uint8_t>(pcAlive);
		payload[4]		  = boot.valid() ? STATUS_FLAG_REMOTE_BOOT_VALID : 0;
		payload[5]		  = static_cast<uint8_t>(id);
		payload[6]		  = static_cast<uint8_t>(id >> 8);
		return 7;
	}

	bool onPcKeepalive(PcSession &session, RxDedup &dedup, const uint8_t *payload, uint8_t len) {
		if (len < 2 || !payload) return false;
		if (!session.observe(static_cast<uint16_t>(payload[0] | (payload[1] << 8)))) return false;
		dedup.forget(protocol::ADDR_PC);
		return true;
	}

	bool RetryQueue::full() const {
		for (const auto &e : _entries) {
			if (!e.used) return false;
		}
		return true;
	}

	bool RetryQueue::add(const protocol::Packet &pkt, uint32_t now) {
		for (auto &e : _entries) {
			if (e.used) continue;
			e = {pkt, now, 0, true, false};
			return true;
		}
		return false;
	}

	bool RetryQueue::resolve(const protocol::Packet &rx, Closed *out) {
		if (rx.payloadLen < 1) return false;
		uint8_t ackedId = rx.payload[0];
		for (auto &e : _entries) {
			if (!e.used || e.pkt.dest != rx.source || e.pkt.packetId != ackedId) continue;
			// PM-6: NAK BUSY = "not now" (PROTOCOL.md): keep the entry, poll() resends it within MAX_RETRIES
			if (rx.msgType == protocol::MsgType::NAK && rx.payloadLen >= 2 && rx.payload[1] == static_cast<uint8_t>(protocol::NakReason::BUSY)) {
				e.busy = true;
				return false;
			}
			e.used = false;
			if (out) {
				out->type	= e.pkt.msgType;
				out->nak	= rx.msgType == protocol::MsgType::NAK;
				out->reason = (out->nak && rx.payloadLen >= 2) ? static_cast<protocol::NakReason>(rx.payload[1]) : protocol::NakReason::UNKNOWN_MSG;
			}
			return true;
		}
		return false;
	}

	const char *failureText(Failure f, protocol::NakReason reason) {
		using protocol::NakReason;
		if (f == Failure::NoAnswer) return "Bez odpovedi";
		if (f == Failure::NotSent) return "Neodeslano"; // retry queue full
		switch (reason) { // same texts as the remote (player_screen.cpp)
			case NakReason::NOT_FOUND: return "Pisen nenalezena";
			case NakReason::BUSY: return "PC zaneprazdneno";
			case NakReason::UNREACHABLE: return "PC nedostupne";
			case NakReason::NOT_PLAYING: return "Nehraje se";
			default: return "Odmitnuto";
		}
	}

} // namespace routercore
