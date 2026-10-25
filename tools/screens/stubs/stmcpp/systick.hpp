#pragma once
// Host stand-in for stmcpp/systick.hpp: the tool sets the time (ticks_ in ms)
#include <cstdint>
#include <stmcpp/units.hpp>

namespace stmcpp::systick {
	using namespace stmcpp::units;

	inline std::uint32_t ticks_ = 0;

	inline std::uint32_t getTicks() { return ticks_; }
	inline duration getDuration() { return 1_ms * ticks_; }
	inline void waitBlocking(duration) {}
}
