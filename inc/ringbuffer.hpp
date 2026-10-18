#ifndef RINGBUFFER_HPP
#define RINGBUFFER_HPP

#include <cstdint>
#include <cstddef>
#include <cmsis_compiler.h>

// Lock-free kruhový buffer pro jednoho producenta a jednoho konzumenta (SPSC).
// Producent zapisuje jen _head, konzument jen _tail, takže stačí, aby jeden z nich
// běžel v přerušení a druhý v hlavní smyčce. Kapacita musí být mocnina dvou.
// Žádná alokace na haldě — vhodné pro ISR.
template<typename T, size_t N>
class RingBuffer {
	static_assert(N >= 2 && (N & (N - 1)) == 0, "Kapacita musi byt mocnina dvou");

public:
	bool push(const T &item) {
		size_t head = _head;
		size_t next = (head + 1) & (N - 1);
		if (next == _tail) return false; // Plno
		_buffer[head] = item;
		__DMB(); // Data musí být zapsána dříve než index
		_head = next;
		return true;
	}

	bool pop(T &item) {
		size_t tail = _tail;
		if (tail == _head) return false; // Prázdno
		__DMB();
		item = _buffer[tail];
		__DMB();
		_tail = (tail + 1) & (N - 1);
		return true;
	}

	// Ukazatel na nejstarší prvek bez vyjmutí (nullptr, když je prázdno)
	T *peek() {
		size_t tail = _tail;
		if (tail == _head) return nullptr;
		__DMB();
		return &_buffer[tail];
	}

	// Zahození nejstaršího prvku (po peek())
	void drop() {
		if (_tail == _head) return;
		_tail = (_tail + 1) & (N - 1);
	}

	bool empty() const { return _head == _tail; }
	size_t size() const { return (_head - _tail) & (N - 1); }
	size_t freeSpace() const { return N - 1 - size(); }
	static constexpr size_t capacity() { return N - 1; }

	// Smí volat jen konzument (nebo obě strany se zakázaným přerušením)
	void clear() { _tail = _head; }

	// Přímý přístup pro DMA: souvislý úsek od _tail (konzument)
	const T *contiguousData(size_t &len) const {
		size_t tail = _tail;
		size_t head = _head;
		len = (head >= tail) ? (head - tail) : (N - tail);
		return &_buffer[tail];
	}

	// Posun _tail o len prvků po dokončení DMA (konzument)
	void consume(size_t len) { _tail = (_tail + len) & (N - 1); }

private:
	T _buffer[N] = {};
	volatile size_t _head = 0;
	volatile size_t _tail = 0;
};

#endif
