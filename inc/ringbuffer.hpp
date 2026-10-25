#ifndef RINGBUFFER_HPP
#define RINGBUFFER_HPP

#include <cstdint>
#include <cstddef>
#include <cmsis_compiler.h>

// Lock-free ring buffer for a single producer and a single consumer (SPSC).
// The producer writes only _head, the consumer only _tail, so it is enough for one of them
// to run in an interrupt and the other in the main loop. The capacity must be a power of two.
// No heap allocation — suitable for ISRs.
template<typename T, size_t N>
class RingBuffer {
	static_assert(N >= 2 && (N & (N - 1)) == 0, "Kapacita musi byt mocnina dvou");

public:
	bool push(const T &item) {
		size_t head = _head;
		size_t next = (head + 1) & (N - 1);
		if (next == _tail) return false; // Full
		_buffer[head] = item;
		__DMB(); // Data must be written before the index
		_head = next;
		return true;
	}

	bool pop(T &item) {
		size_t tail = _tail;
		if (tail == _head) return false; // Empty
		__DMB();
		item = _buffer[tail];
		__DMB();
		_tail = (tail + 1) & (N - 1);
		return true;
	}

	// Pointer to the oldest element without removing it (nullptr when empty)
	T *peek() {
		size_t tail = _tail;
		if (tail == _head) return nullptr;
		__DMB();
		return &_buffer[tail];
	}

	// Drops the oldest element (after peek())
	void drop() {
		if (_tail == _head) return;
		_tail = (_tail + 1) & (N - 1);
	}

	bool empty() const { return _head == _tail; }
	size_t size() const { return (_head - _tail) & (N - 1); }
	size_t freeSpace() const { return N - 1 - size(); }
	static constexpr size_t capacity() { return N - 1; }

	// May be called only by the consumer (or by both sides with interrupts disabled)
	void clear() { _tail = _head; }

	// Direct access for DMA: contiguous run starting at _tail (consumer)
	const T *contiguousData(size_t &len) const {
		size_t tail = _tail;
		size_t head = _head;
		len = (head >= tail) ? (head - tail) : (N - tail);
		return &_buffer[tail];
	}

	// Advances _tail by len elements after the DMA completes (consumer)
	void consume(size_t len) { _tail = (_tail + len) & (N - 1); }

private:
	T _buffer[N] = {};
	volatile size_t _head = 0;
	volatile size_t _tail = 0;
};

#endif
