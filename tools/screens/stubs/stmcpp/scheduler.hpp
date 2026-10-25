#pragma once
// Host stand-in for stmcpp/scheduler.hpp: schedulers never fire, the tool calls the GUI directly
#include <stmcpp/units.hpp>

namespace stmcpp::scheduler {
	using namespace stmcpp::units;

	class Scheduler {
	  public:
		Scheduler(duration, void (*)(), bool = false, bool = false) {}
		void dispatch() {}
		void start() {}
		void stop() {}
	};
}
