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
