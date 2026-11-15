#include "ram.hpp"
#include "logic/ram_guard.hpp"

#include <cerrno>

#include <stm32g431xx.h>
#include <core_cm4.h>

// Linker symbols (STM32G431CBTx_FLASH.ld): heap start, top of RAM, stack reserve
extern "C" char end;
extern "C" char _estack;
extern "C" char _Min_Stack_Size;

namespace {
	char			 *_brk		= &end;
	volatile uint32_t _failures = 0;

	uintptr_t heapStart() { return reinterpret_cast<uintptr_t>(&end); }

	// The heap may grow up to _estack - _Min_Stack_Size: the stack reserve of the linker script is never handed out
	uintptr_t limit() { return reinterpret_cast<uintptr_t>(&_estack) - reinterpret_cast<uintptr_t>(&_Min_Stack_Size); }

	// First whole word above the heap break
	const uint32_t *breakWord() { return reinterpret_cast<const uint32_t *>((reinterpret_cast<uintptr_t>(_brk) + 3u) & ~uintptr_t{3}); }
} // namespace

// Replaces the libnosys _sbrk (no limit, the heap could silently run into the stack). On refusal malloc returns
// nullptr; operator new then calls abort() → _exit() spins → loop watchdog (1 s) → LOOP_STALL record + reset, SSR off.
extern "C" void *_sbrk(ptrdiff_t incr) {
	uintptr_t brk = reinterpret_cast<uintptr_t>(_brk);
	if (!ramguard::breakAllowed(brk, incr, limit(), heapStart())) {
		_failures = _failures + 1;
		errno	  = ENOMEM;
		return reinterpret_cast<void *>(-1);
	}
	char *prev = _brk;
	_brk += incr;
	return prev;
}

namespace ram {

	[[gnu::noinline]] void paintStack() {
		// Up to 64 B below the current SP (this frame and the caller stay untouched)
		uint32_t *p  = const_cast<uint32_t *>(breakWord());
		uint32_t *hi = reinterpret_cast<uint32_t *>((__get_MSP() - 64u) & ~uint32_t{3});
		while (p < hi) *p++ = ramguard::PAINT;
	}

	size_t stackUntouched() {
		return ramguard::untouchedBytes(breakWord(), reinterpret_cast<const uint32_t *>(reinterpret_cast<uintptr_t>(&_estack)));
	}

	size_t heapUsed() { return static_cast<size_t>(reinterpret_cast<uintptr_t>(_brk) - heapStart()); }

	size_t heapLimit() { return static_cast<size_t>(limit() - heapStart()); }

	uint32_t sbrkFailures() { return _failures; }

} // namespace ram
