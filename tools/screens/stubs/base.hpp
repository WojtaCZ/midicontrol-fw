#pragma once
// Host stand-in for inc/base.hpp (the real one pulls in the STM32 device header)
#include <string>

namespace base {
	static std::string DEVICE_NAME = "MIDIControl";
	static std::string DEVICE_TYPE = "BASE";
	static std::string FW_VERSION  = "2.0.0";

	namespace current {
		bool isEnabled();
		void enable();
		void disable();
	}
}
