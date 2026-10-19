#ifndef DEBUG_HPP
#define DEBUG_HPP

#include <cstdint>

// Textový log base: řádky "[ssssss.mmm] L tag: text\r\n" do kruhového bufferu v no-init RAM (přežije reset,
// ne výpadek napájení); hlavní smyčka (process) je posílá do druhého USB-CDC ("MIDIControl log", CDC 1).
// Protokolové CDC (CDC 0) nese dál jen protokolové rámce.
namespace Debug {

	enum class Level : uint8_t { NONE = 0, ERROR, WARNING, INFO, DEBUG };

	// Události z přerušení: ISR jen zvýší čítač (logFromIsr), řádek z nich udělá process()
	enum IsrEvent : uint8_t {
		ISR_BLE_UART_ERROR = 0, // USART2 ORE/FE/NE (RN4870)
		ISR_BLE_RX_OVERFLOW,    // plný RX ring BLE
		ISR_BLE_STATUS_OVERFLOW, // plná fronta stavových zpráv RN4870
		ISR_MIDI_UART_ERROR,    // USART3 ORE/FE/NE (DIN MIDI)
		ISR_MIDI_OVERFLOW,      // plná fronta DIN → USB
		ISR_LOG_FROM_ISR,       // Debug::log zavolaný z přerušení (zahozeno)
		ISR_EVENT_COUNT
	};

	// Jen z hlavní smyčky (nikdy z ISR: zápis do ringu není reentrantní). Z ISR → logFromIsr.
	// tag: modul, nejvýše 6 znaků (sys ble router usb disp midi set ota). Text nad 120 znaků se zkrátí a končí '~'.
	void log(Level lvl, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
	// Bezpečné z ISR: jen čítač události; process() z něj udělá řádek
	void logFromIsr(uint8_t eventCode);
	// Ověří hlavičku ringu v .noinit; platná → ponechá obsah z doby před resetem, neplatná → vymaže
	void init();
	// Úvodní řádky (úroveň I, vždy): firmware, revize/datum, příčina resetu, uložený záznam o chybě (Task 6C)
	void banner();
	// Hlavní smyčka: posílá ring do CDC 1, je-li port otevřený (DTR); příkazy "level x", "dump", "help". Nikdy neblokuje.
	void process();
	Level getLevel();
	void setLevel(Level level);

}

#endif
