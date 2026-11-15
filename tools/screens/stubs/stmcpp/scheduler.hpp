#pragma once
// Host stand-in for stmcpp/scheduler.hpp. One-shot schedulers (song list delay/timeout, splash
// dismiss) fire when the driver advances time with Scheduler::advance(ms); repeating ones
// (render, keypress) never fire, the tool calls the GUI directly.
#include <stmcpp/units.hpp>
#include <cstdint>
#include <vector>

namespace stmcpp::scheduler {
	using namespace stmcpp::units;

	class Scheduler {
	  public:
		Scheduler(duration period, void (*fn)(), bool repeat = false, bool autostart = false)
			: ms_(static_cast<std::uint32_t>(period.toMilliSeconds() + 0.5f)), fn_(fn), repeat_(repeat), active_(autostart && !repeat) {
			left_ = ms_;
			all().push_back(this);
		}
		void dispatch() {}
		void start() { active_ = !repeat_; left_ = ms_; }
		void stop() { active_ = false; }

		// Time passes: fires the one-shots that run out (in start order of the list)
		static void advance(std::uint32_t ms) {
			for (auto *s : all()) {
				if (!s->active_) continue;
				if (ms >= s->left_) {
					s->active_ = false;
					s->left_ = 0;
					if (s->fn_) s->fn_();
				} else {
					s->left_ -= ms;
				}
			}
		}

	  private:
		static std::vector<Scheduler *> &all() {
			static std::vector<Scheduler *> v;
			return v;
		}
		std::uint32_t ms_, left_;
		void (*fn_)();
		bool repeat_, active_;
	};
}
