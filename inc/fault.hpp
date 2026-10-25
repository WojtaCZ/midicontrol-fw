#ifndef FAULT_HPP
#define FAULT_HPP

#include <cstddef>
#include <cstdint>

// Base fault record (diagnostics spec §2): HardFault/MemManage/BusFault/UsageFault/NMI, signalError(),
// stmcpp::error and the loop watchdog (wdg) write one record into no-init RAM (.noinit, next to the log; survives
// a reset, not a power loss) and a line into the log. Then: debug probe attached (DHCSR.C_DEBUGEN) → bkpt, otherwise
// NVIC_SystemReset(). The next boot prints the record in the banner (Debug::banner), the "dump" command on the log CDC repeats it.
namespace fault {

	enum class Kind : uint8_t { HARDFAULT = 1, MEMMANAGE, BUSFAULT, USAGEFAULT, ERROR, LOOP_STALL };

	// Codes for Kind::ERROR (errorCode); stmcpp::error → 0x8000 | module error code
	enum ErrorCode : uint16_t {
		ERR_BLE_INIT	  = 1, // RN4870 without %REBOOT% after reset
		ERR_BLE_CONFIGURE = 2, // the RN4870 configuration could not be queued
		ERR_STMCPP		  = 0x8000,
	};

	struct Record { // in .noinit, valid only with magic 0x46415554 ("TUAF") and a correct crc
		uint32_t magic;
		uint8_t	 kind; // Kind
		uint8_t	 count; // number of faults since power-on (1 = first), saturates at 255
		uint16_t errorCode; // Kind::ERROR: ErrorCode
		// Saved exception frame (r0–r3, r12, lr, pc, xpsr) from the stack selected by EXC_RETURN; for ERROR r0 = code
		uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
		uint32_t excReturn, msp, psp, ipsr;
		uint32_t cfsr, hfsr, mmfar, bfar, afsr; // SCB at the time of the fault
		uint32_t uptimeMs;
		char	 rev[8]; // firmware git revision (7 characters + NUL)
		uint32_t crc; // CRC-32 of all preceding bytes
	};
	static_assert(sizeof(Record) == 92, "tools/basetest/dbg.py FAULT_RECORD_SIZE");
	static_assert(offsetof(Record, crc) == 88);

	// Fault injection for hardware tests (tools/basetest/test_fault.py): the main loop reads it once per iteration.
	// 1 = read from 0xFFFFFFF0 (BusFault), 2 = loop with interrupts enabled (loop stall), 3 = loop with
	// interrupts disabled (IWDG). Stays in the release build too (1 byte, one comparison).
	extern volatile uint8_t testInject;
	[[noreturn]] void		testSpin();
	[[noreturn]] void		testSpinNoIrq();

	// Enables the separate MemManage/BusFault/UsageFault (otherwise everything ends up as a HardFault FORCED)
	void init();
	// Writes the record (stackedFrame = 8 words r0..xpsr, or nullptr) and a log line; nothing else
	void record(Kind kind, const uint32_t *stackedFrame, uint32_t excReturn, uint16_t errorCode = 0);
	// Non-terminating runtime error (e.g. RN4870, spec R21): ERROR record + log line, no finish()/reset, immediately
	// marked as announced (the next boot does not report it as an error of the previous run). An older record that has not yet been
	// announced or is a real fault (not ERROR) is not overwritten.
	void recordNonFatal(uint16_t errorCode, uint32_t caller);
	// Probe attached → bkpt (reset after continuing), otherwise NVIC_SystemReset()
	[[noreturn]] void finish();
	// record + finish
	[[noreturn]] void capture(Kind kind, const uint32_t *stackedFrame, uint32_t excReturn, uint16_t errorCode = 0);

	const Record *saved(); // nullptr when none / invalid
	void		  clear();
	// The banner announces a record only once (level E); on later boots only a reference (I)
	bool announced();
	void markAnnounced();

	const char *kindName(uint8_t kind);
	// Line idx (0..) of the record's text dump into buf; returns the length, 0 = no more lines
	size_t formatLine(const Record &r, uint8_t idx, char *buf, size_t size);

} // namespace fault

#endif
