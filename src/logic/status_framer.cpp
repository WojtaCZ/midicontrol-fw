#include "status_framer.hpp"

namespace statusframe {

	Result Framer::feed(uint8_t c, uint32_t nowMs) {
		if (active_ && nowMs - lastMs_ > GAP_MS) {
			// A status never pauses: the opening '%' was really a lost closer. Treat this byte as a fresh one
			active_ = false;
			len_ = 0;
			resyncs_++;
		}
		lastMs_ = nowMs;
		if (!active_) {
			if (c != static_cast<uint8_t>(DELIMITER)) return Result::NOT_STATUS;
			active_ = true;
			len_ = 0;
			return Result::CONSUMED;
		}
		if (c == static_cast<uint8_t>(DELIMITER)) {
			// "%%": the module never sends an empty status, so this is a closing '%' whose opener was lost followed by a new
			// status, or an opener after a lost closer: stay inside and take what follows as the next status
			if (len_ == 0) return Result::CONSUMED;
			text_[len_] = '\0';
			active_ = false;
			return Result::COMPLETE;
		}
		if (c < 0x20 || c > 0x7E) {
			// Not status text: hand the byte back
			active_ = false;
			len_ = 0;
			resyncs_++;
			return Result::NOT_STATUS;
		}
		if (len_ < TEXT_LEN - 1) text_[len_++] = static_cast<char>(c); // longer statuses (scan results) are truncated
		return Result::CONSUMED;
	}

}
