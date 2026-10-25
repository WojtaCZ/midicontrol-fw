#ifndef DEBUG_HPP
#define DEBUG_HPP

#include <cstdint>

// Base text log: lines "[ssssss.mmm] L tag: text\r\n" into a ring buffer in no-init RAM (survives a reset,
// not a power loss); the main loop (process) sends them to the second USB-CDC ("MIDIControl log", CDC 1).
// The protocol CDC (CDC 0) still carries only protocol frames.
namespace Debug {

	enum class Level : uint8_t { NONE = 0, ERROR, WARNING, INFO, DEBUG };

	// Events from interrupts: the ISR only increments a counter (logFromIsr), process() turns it into a line
	enum IsrEvent : uint8_t {
		ISR_BLE_UART_ERROR = 0, // USART2 ORE/FE/NE (RN4870)
		ISR_BLE_RX_OVERFLOW,    // BLE RX ring full
		ISR_BLE_STATUS_OVERFLOW, // RN4870 status message queue full
		ISR_MIDI_UART_ERROR,    // USART3 ORE/FE/NE (DIN MIDI)
		ISR_MIDI_OVERFLOW,      // DIN → USB queue full
		ISR_LOG_FROM_ISR,       // Debug::log called from an interrupt (dropped)
		ISR_EVENT_COUNT
	};

	// Main loop only (never from an ISR: writing to the ring is not reentrant). From an ISR → logFromIsr.
	// tag: module, at most 6 characters (sys ble router usb disp midi set ota). Text over 120 characters is truncated and ends with '~'.
	void log(Level lvl, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
	// Safe from an ISR: only an event counter; process() turns it into a line
	void logFromIsr(uint8_t eventCode);
	// Only from the error path (fault::record, interrupts disabled, reset follows): an "E fault: ..." line into the ring without an IPSR check
	void logFault(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
	// Validates the ring header in .noinit; valid → keeps the content from before the reset, invalid → clears it
	void init();
	// Opening lines (level I, always): firmware, revision/date, reset cause, saved fault record (fault::saved)
	void banner();
	// Main loop: sends the ring to CDC 1 if the port is open (DTR); commands "level x", "dump", "help". Never blocks.
	void process();
	Level getLevel();
	void setLevel(Level level);

}

#endif
