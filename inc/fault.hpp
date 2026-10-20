#ifndef FAULT_HPP
#define FAULT_HPP

#include <cstddef>
#include <cstdint>

// Záznam o chybě base (spec diagnostiky §2): HardFault/MemManage/BusFault/UsageFault/NMI, signalError(),
// stmcpp::error a smyčkový watchdog (wdg) zapíší jeden záznam do no-init RAM (.noinit, vedle logu; přežije
// reset, ne výpadek napájení) a řádek do logu. Pak: ladicí sonda připojena (DHCSR.C_DEBUGEN) → bkpt, jinak
// NVIC_SystemReset(). Další boot vypíše záznam v banneru (Debug::banner), příkaz "dump" na log CDC ho zopakuje.
namespace fault {

	enum class Kind : uint8_t { HARDFAULT = 1, MEMMANAGE, BUSFAULT, USAGEFAULT, ERROR, LOOP_STALL };

	// Kódy pro Kind::ERROR (errorCode); stmcpp::error → 0x8000 | kód chyby modulu
	enum ErrorCode : uint16_t {
		ERR_BLE_INIT	  = 1, // RN4870 bez %REBOOT% po resetu
		ERR_BLE_CONFIGURE = 2, // konfiguraci RN4870 nešlo zařadit
		ERR_STMCPP		  = 0x8000,
	};

	struct Record { // v .noinit, platný jen s magic 0x46415554 ("TUAF") a správným crc
		uint32_t magic;
		uint8_t	 kind; // Kind
		uint8_t	 count; // počet chyb od zapnutí napájení (1 = první), saturuje na 255
		uint16_t errorCode; // Kind::ERROR: ErrorCode
		// Uložený rámec výjimky (r0–r3, r12, lr, pc, xpsr) ze zásobníku podle EXC_RETURN; u ERROR r0 = kód
		uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
		uint32_t excReturn, msp, psp, ipsr;
		uint32_t cfsr, hfsr, mmfar, bfar, afsr; // SCB v okamžiku chyby
		uint32_t uptimeMs;
		char	 rev[8]; // git revize firmware (7 znaků + NUL)
		uint32_t crc; // CRC-32 všech předchozích bajtů
	};
	static_assert(sizeof(Record) == 92, "tools/basetest/dbg.py FAULT_RECORD_SIZE");
	static_assert(offsetof(Record, crc) == 88);

	// Injekce chyb pro hardwarové testy (tools/basetest/test_fault.py): hlavní smyčka čte jednou za průchod.
	// 1 = čtení z 0xFFFFFFF0 (BusFault), 2 = smyčka s povolenými přerušeními (loop stall), 3 = smyčka se
	// zakázanými přerušeními (IWDG). Zůstává i v release buildu (1 bajt, jedno porovnání).
	extern volatile uint8_t testInject;
	[[noreturn]] void		testSpin();
	[[noreturn]] void		testSpinNoIrq();

	// Povolí samostatné MemManage/BusFault/UsageFault (jinak vše končí jako HardFault FORCED)
	void init();
	// Zapíše záznam (stackedFrame = 8 slov r0..xpsr, nebo nullptr) a řádek do logu; nic dalšího
	void record(Kind kind, const uint32_t *stackedFrame, uint32_t excReturn, uint16_t errorCode = 0);
	// Neukončující chyba za běhu (např. RN4870, spec R21): záznam ERROR + řádek logu, bez finish()/resetu, rovnou
	// označený jako ohlášený (další boot ho nevydává za chybu minulého běhu). Starší záznam, který ještě nebyl
	// ohlášen nebo je skutečná chyba (ne ERROR), se nepřepisuje.
	void recordNonFatal(uint16_t errorCode, uint32_t caller);
	// Sonda připojena → bkpt (po pokračování reset), jinak NVIC_SystemReset()
	[[noreturn]] void finish();
	// record + finish
	[[noreturn]] void capture(Kind kind, const uint32_t *stackedFrame, uint32_t excReturn, uint16_t errorCode = 0);

	const Record *saved(); // nullptr, když žádný / neplatný
	void		  clear();
	// Banner ohlašuje záznam jen jednou (úroveň E); při dalších bootech už jen odkaz (I)
	bool announced();
	void markAnnounced();

	const char *kindName(uint8_t kind);
	// Řádek idx (0..) textového výpisu záznamu do buf; vrací délku, 0 = žádné další řádky
	size_t formatLine(const Record &r, uint8_t idx, char *buf, size_t size);

} // namespace fault

#endif
