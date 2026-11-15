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
		LastRx &last = _last[source];
		if (last.valid && last.packetId == packetId) return true;
		last.packetId = packetId;
		last.valid	  = true;
		return false;
	}

	void RxDedup::forget(uint8_t source) {
		if (source < NODES) _last[source].valid = false;
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
		if (f == Failure::NoAnswer) return "PC neodpovida";
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
