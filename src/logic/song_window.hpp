#pragma once
#include <cstdint>
// A9 / B5: paging state of the song list on the base. The list has no firmware cap (the PC's sorted list can be any
// length); the base keeps a WINDOW of CAP entries and remembers the absolute index (index in the PC's list) of item 0.
// Requests carry an absolute offset (u16, protocol v2 GET_SONG_LIST): Down near the end of the window asks for
// start + count, Up at the first item asks for max(0, start - UP_STEP). A response is applied by absolute index:
// the page may hold more or fewer entries than expected, the window is rebuilt from what arrives and the cursor
// stays on the same absolute entry (Up: the previous one). The offset always advances by the number of codes
// RECEIVED and kept (RF-18); codes of a response that do not fit are dropped and hasMore is forced on so that the
// next request starts right after the last kept one.
// Hardware-free; the menu owns the name storage and applies the Plan.

namespace songwin {

	class Window {
	public:
		static constexpr int CAP = 24;		// entries held (song items, the "Zpet" item is extra)
		static constexpr int UP_STEP = 12;	// how far back an Up request starts
		static constexpr int PREFETCH = 2;	// Down asks for more when the cursor is within this many of the end

		enum class State : uint8_t { Idle, WaitingFirst, WaitingMore };

		// How the menu must rebuild its entry array for one response.
		struct Plan {
			bool ok = false;	 // false: no request was pending, ignore the response
			int dropFront = 0;	 // old entries to remove from the front (all of them for First/Up)
			int keepNew = 0;	 // first N codes of the response to store after the kept old entries
			int count = 0;		 // entries in the window afterwards
			int cursor = 0;		 // song item to select afterwards (0..count-1)
		};

		State state() const { return _state; }
		uint16_t windowStart() const { return _start; }
		int count() const { return _count; }
		bool hasMore() const { return _hasMore; }

		// New list: returns the offset of the first request (0).
		uint16_t startFirst() {
			_state = State::WaitingFirst;
			_start = 0;
			_count = 0;
			_hasMore = false;
			return 0;
		}

		// Down was pressed and the cursor is now on item `sel` (0-based, may be the "Zpet" item = count).
		bool downRequest(int sel, uint16_t& offset) {
			if (_state != State::Idle || !_hasMore || _count <= 0) return false;
			if (sel < _count - PREFETCH) return false;
			_dir = Dir::Down;
			_cursorAbs = static_cast<uint16_t>(_start + (sel < _count ? sel : _count - 1));
			offset = static_cast<uint16_t>(_start + _count);
			_state = State::WaitingMore;
			return true;
		}

		// Up was pressed while the cursor was on item `selBefore`: at the first item with earlier entries -> ask.
		bool upRequest(int selBefore, uint16_t& offset) {
			if (_state != State::Idle || selBefore != 0 || _start == 0 || _count <= 0) return false;
			_dir = Dir::Up;
			_cursorAbs = _start - 1;
			offset = _start > UP_STEP ? static_cast<uint16_t>(_start - UP_STEP) : 0;
			_reqStart = offset;
			_state = State::WaitingMore;
			return true;
		}

		// A response with `codes` entries arrived (counted by the caller, empty entries excluded).
		Plan onResponse(int codes, bool pcHasMore) {
			Plan p;
			if (_state == State::Idle) return p;
			p.ok = true;
			const bool first = _state == State::WaitingFirst;
			_state = State::Idle;
			const int kept = codes < CAP ? codes : CAP;
			const bool truncated = codes > kept;

			if (first) {
				p.dropFront = _count;
				p.keepNew = kept;
				p.count = kept;
				p.cursor = 0;
				_start = 0;
				_count = kept;
				_hasMore = pcHasMore || truncated;
				return p;
			}
			if (codes <= 0) {
				// nothing new: Down reached the end, Up got nothing (the window stays as it is)
				if (_dir == Dir::Down) _hasMore = false;
				p.count = _count;
				p.cursor = clampAbs(_cursorAbs, _start, _count);
				return p;
			}
			if (_dir == Dir::Down) {
				int total = _count + kept;
				int drop = total > CAP ? total - CAP : 0;
				p.dropFront = drop;
				p.keepNew = kept;
				_start = static_cast<uint16_t>(_start + drop);
				_count = total - drop;
			} else {
				p.dropFront = _count;
				p.keepNew = kept;
				_start = _reqStart;
				_count = kept;
			}
			p.count = _count;
			p.cursor = clampAbs(_cursorAbs, _start, _count);
			_hasMore = pcHasMore || truncated;
			return p;
		}

		// Timeout, NAK, send failure or the screen was left: the next scroll asks again.
		void abort() { _state = State::Idle; }

		// The list screen was left or the base reset: forget the window.
		void reset() {
			_state = State::Idle;
			_start = 0;
			_count = 0;
			_hasMore = false;
		}

	private:
		enum class Dir : uint8_t { Down, Up };

		static int clampAbs(uint16_t abs, uint16_t start, int count) {
			int idx = static_cast<int>(abs) - static_cast<int>(start);
			if (idx < 0) idx = 0;
			if (idx > count - 1) idx = count - 1;
			return idx < 0 ? 0 : idx;
		}

		State _state = State::Idle;
		Dir _dir = Dir::Down;
		uint16_t _start = 0;	  // absolute index of item 0
		uint16_t _reqStart = 0;	  // start the pending Up request asked for
		uint16_t _cursorAbs = 0;  // absolute entry the cursor goes back to
		int _count = 0;
		bool _hasMore = false;
	};

} // namespace songwin
