#include "router_core.hpp"

namespace routercore {

	uint32_t msFromTicks(uint32_t ticks, uint32_t tickUs) {
		// As stmcpp::systick::getDuration() / 1_ms: the product is a uint32 count of µs
		return (ticks * tickUs) / 1000u;
	}

	bool deadlineReached(uint32_t now, uint32_t deadline) {
		return now >= deadline;
	}

	void PeerPresence::onRx(uint32_t now) {
		_lastRx	  = now;
		_now	  = now;
		_everSeen = true;
	}

	void PeerPresence::update(uint32_t now) {
		_now = now;
	}

	bool PeerPresence::alive() const {
		return _everSeen && (_now - _lastRx) < _loss;
	}

	bool RxDedup::isRepeat(uint8_t source, uint8_t packetId) {
		if (source >= NODES) return false;
		LastRx &last = _last[source];
		if (last.valid && last.packetId == packetId) return true;
		last.packetId = packetId;
		last.valid	  = true;
		return false;
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
			e = {pkt, now, 0, true};
			return true;
		}
		return false;
	}

	void RetryQueue::resolve(const protocol::Packet &rx) {
		if (rx.payloadLen < 1) return;
		uint8_t ackedId = rx.payload[0];
		for (auto &e : _entries) {
			if (e.used && e.pkt.dest == rx.source && e.pkt.packetId == ackedId) e.used = false;
		}
	}

} // namespace routercore
