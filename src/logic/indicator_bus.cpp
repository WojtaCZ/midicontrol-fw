#include "indicator_bus.hpp"

namespace indicator {
	bool frameAccepted(const uint8_t* frame, size_t len) {
		return frame != nullptr && len == FRAME_LEN && frame[0] == FLAG;
	}
}
