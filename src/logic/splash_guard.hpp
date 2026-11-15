#pragma once
// P1-8: the splash's 3 s auto-dismiss is a synthetic Enter. It must not fire once the user has pressed any key
// (the stale timer would otherwise press Enter on whatever screen the user has reached by then).

namespace splashguard {

	class Guard {
	public:
		void realKey() { _realKey = true; }				// any key from the encoder / buttons
		bool allowAutoDismiss() const { return !_realKey; } // the timer may inject its Enter

	private:
		bool _realKey = false;
	};

} // namespace splashguard
