#pragma once
#include <cstdint>
// BE-1 / SYS-LINK-01: an accepted BLE link that carries no frame from the remote (the remote answers every 2 s KEEPALIVE)
// must not stay "connected" forever. Both sides kept reporting a link for 20+ s with nothing flowing (audit step 3, be1.md);
// before this guard the base only marked the remote "not alive" and never dropped the link, so auto-reconnect never ran.
// STALE_MS without a remote frame (counted from the later of the last frame and the moment the link became accepted) → KICK
// (the caller sends K,1; auto-reconnect follows). Still accepted and silent RESTART_AFTER_MS after the kick → RESTART (module RST,
// once per stale episode). A frame after the kick, or the link going inactive, ends the episode.
// Times are ms from a free-running uint32 clock; only modular differences are used (wraps after 49.7 days).

namespace stalelink {

	enum class Action : uint8_t { NONE, KICK, RESTART };

	class Guard {
	public:
		static constexpr uint32_t STALE_MS = 10000;		  // > KEEPALIVE_LOSS_MS (6 s): 4 missed keepalive answers
		static constexpr uint32_t RESTART_AFTER_MS = 5000; // K,1 normally ends the link in well under a second

		// active: module READY && connected && peer accepted && not pairing.
		// rxSeen/lastRxMs: the last frame received over BLE (rxSeen false = never since boot).
		Action update(bool active, bool rxSeen, uint32_t lastRxMs, uint32_t nowMs) {
			if (!active) {
				_state = State::IDLE;
				return Action::NONE;
			}
			if (_state == State::IDLE) {
				_state = State::WATCH;
				_since = nowMs;
				return Action::NONE;
			}
			const bool rxAfterSince = rxSeen && static_cast<int32_t>(lastRxMs - _since) > 0;
			switch (_state) {
				case State::WATCH: {
					uint32_t ref = rxAfterSince ? lastRxMs : _since;
					if (nowMs - ref < STALE_MS) return Action::NONE;
					_state = State::KICKED;
					_since = nowMs;
					return Action::KICK;
				}
				case State::KICKED:
					if (rxAfterSince) { // the link came back by itself
						_state = State::WATCH;
						_since = lastRxMs;
						return Action::NONE;
					}
					if (nowMs - _since < RESTART_AFTER_MS) return Action::NONE;
					_state = State::RESTARTED;
					return Action::RESTART;
				default: return Action::NONE; // RESTARTED: wait until the link goes inactive
			}
		}

	private:
		enum class State : uint8_t { IDLE, WATCH, KICKED, RESTARTED };
		State _state = State::IDLE;
		uint32_t _since = 0; // WATCH: link accepted; KICKED: kick time
	};

} // namespace stalelink
