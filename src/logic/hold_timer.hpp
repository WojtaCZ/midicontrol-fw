#pragma once
// Button timing shared by the base and the remote (identical file in both repos).
//  - holdtimer::Timer: "hold to confirm" (power off, emergency off). tick() with the raw button state every period;
//    percent() drives the loading bar of the HoldScreen (inc/hold_screen.hpp); tick() is true once when the hold is complete.
//  - holdtimer::PressTracker: a short press (reported on RELEASE) and a long press (reported once while still held).
//    The base encoder uses it so that a long press can leave a list without first selecting an item.

#include <cstdint>

namespace holdtimer {

	constexpr uint32_t HOLD_MS = 3000; // owner decision 2026-11-23: every hold-to-confirm takes 3 s on both devices

	class Timer {
	public:
		explicit constexpr Timer(uint32_t holdMs = HOLD_MS) : _holdMs(holdMs) {}

		// True exactly once per hold, in the period that reaches holdMs. Releasing starts over.
		constexpr bool tick(bool pressed, uint32_t dtMs) {
			if (!pressed) {
				_heldMs = 0;
				_fired	= false;
				return false;
			}
			if (_fired) return false;
			_heldMs += dtMs;
			if (_heldMs >= _holdMs) {
				_heldMs = _holdMs;
				_fired	= true;
				return true;
			}
			return false;
		}

		constexpr void reset() { _heldMs = 0; _fired = false; }
		constexpr uint32_t heldMs() const { return _heldMs; }
		constexpr uint8_t percent() const { return static_cast<uint8_t>(static_cast<uint64_t>(_heldMs) * 100u / _holdMs); }
		constexpr uint32_t holdMs() const { return _holdMs; }

	private:
		uint32_t _holdMs;
		uint32_t _heldMs = 0;
		bool	 _fired	 = false;
	};

	enum class Press : uint8_t { None, Short, Long };

	class PressTracker {
	public:
		static constexpr uint32_t DEBOUNCE_MS = 20;	 // shorter presses are contact noise
		static constexpr uint32_t LONG_MS	  = 800; // Long fires at this point, while the button is still down

		// Call every period (10 ms on the base). Short comes on release, Long once at LONG_MS (no Short after it).
		constexpr Press tick(bool pressed, uint32_t dtMs) {
			if (_ignore) {
				if (!pressed) _ignore = false;
				return Press::None;
			}
			if (pressed) {
				_heldMs += dtMs;
				if (!_longSent && _heldMs >= LONG_MS) {
					_longSent = true;
					return Press::Long;
				}
				return Press::None;
			}
			const bool shortPress = _heldMs >= DEBOUNCE_MS && !_longSent;
			_heldMs				  = 0;
			_longSent			  = false;
			return shortPress ? Press::Short : Press::None;
		}

		// A button that is down at power-up (or after a wake-up) must be released before it counts
		constexpr void ignoreUntilRelease() { _ignore = true; _heldMs = 0; _longSent = false; }
		constexpr uint32_t heldMs() const { return _heldMs; }

	private:
		uint32_t _heldMs   = 0;
		bool	 _longSent = false;
		bool	 _ignore   = false;
	};

} // namespace holdtimer
