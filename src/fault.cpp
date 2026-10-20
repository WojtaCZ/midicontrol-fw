#include "fault.hpp"
#include "crc32.hpp"
#include "debug.hpp"
#include "git.hpp"

#include <cstdio>
#include <cstring>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

#include "stmcpp/systick.hpp"

namespace fault {

	static constexpr uint32_t MAGIC = 0x46415554; // "TUAF"

	// used: testy (dbg.fault_record) ho čtou sondou podle tabulky symbolů
	[[gnu::section(".noinit.fault"), gnu::used]] Record _record;
	// CRC záznamu, který už banner ohlásil (úroveň E); jiný/nový záznam se ohlásí znovu
	[[gnu::section(".noinit.fault"), gnu::used]] uint32_t _announcedCrc;

	[[gnu::used]] volatile uint8_t testInject = 0;

	static uint32_t recordCrc(const Record &r) { return util::crc32(&r, offsetof(Record, crc)); }

	const Record *saved() {
		if (_record.magic != MAGIC || _record.crc != recordCrc(_record)) return nullptr;
		return &_record;
	}

	void clear() {
		memset(&_record, 0, sizeof(_record));
		_announcedCrc = 0;
	}

	bool announced() {
		const Record *r = saved();
		return r && _announcedCrc == r->crc;
	}

	void markAnnounced() {
		if (const Record *r = saved()) _announcedCrc = r->crc;
	}

	void init() {
		// Samostatné handlery: přesnější druh chyby (jinak HardFault s HFSR.FORCED a stejným CFSR)
		SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
	}

	const char *kindName(uint8_t kind) {
		switch (static_cast<Kind>(kind)) {
			case Kind::HARDFAULT: return "HARDFAULT";
			case Kind::MEMMANAGE: return "MEMMANAGE";
			case Kind::BUSFAULT: return "BUSFAULT";
			case Kind::USAGEFAULT: return "USAGEFAULT";
			case Kind::ERROR: return "ERROR";
			case Kind::LOOP_STALL: return "LOOP_STALL";
		}
		return "UNKNOWN";
	}

	// Rámec musí ležet v RAM (r0..xpsr = 8 slov), jinak by jeho čtení v HardFaultu skončilo lockupem
	static bool frameValid(const uint32_t *f) {
		uint32_t a = reinterpret_cast<uint32_t>(f);
		return f && (a & 3) == 0 && a >= 0x20000000 && a + 32 <= 0x20008000;
	}

	[[gnu::noinline]] void record(Kind kind, const uint32_t *frame, uint32_t excReturn, uint16_t errorCode) {
		uint32_t primask = __get_PRIMASK();
		__disable_irq();
		const Record *prev	= saved();
		uint8_t		  count = prev ? static_cast<uint8_t>(prev->count == 255 ? 255 : prev->count + 1) : 1;

		Record r	= {};
		r.magic		= MAGIC;
		r.kind		= static_cast<uint8_t>(kind);
		r.count		= count;
		r.errorCode = errorCode;
		if (frameValid(frame)) {
			r.r0   = frame[0];
			r.r1   = frame[1];
			r.r2   = frame[2];
			r.r3   = frame[3];
			r.r12  = frame[4];
			r.lr   = frame[5];
			r.pc   = frame[6];
			r.xpsr = frame[7];
		}
		r.excReturn = excReturn;
		// Ukazatel zásobníku v okamžiku výjimky = adresa uloženého rámce na zásobníku podle EXC_RETURN bit 2
		bool onPsp = excReturn & 4u;
		r.msp	   = (frame && !onPsp) ? reinterpret_cast<uint32_t>(frame) : __get_MSP();
		r.psp	   = (frame && onPsp) ? reinterpret_cast<uint32_t>(frame) : __get_PSP();
		r.ipsr	   = __get_IPSR();
		r.cfsr	   = SCB->CFSR;
		r.hfsr	   = SCB->HFSR;
		r.mmfar	   = SCB->MMFAR;
		r.bfar	   = SCB->BFAR;
		r.afsr	   = SCB->AFSR;
		r.uptimeMs = stmcpp::systick::getTicks();
		strncpy(r.rev, git::revision.c_str(), sizeof(r.rev) - 1);
		r.crc	= recordCrc(r);
		_record = r;

		// Řádek do logu (no-init ring): po resetu ho ukáže "log before reset"
		Debug::logFault("%s #%u pc=0x%08lx lr=0x%08lx code %u", kindName(r.kind), static_cast<unsigned>(r.count), static_cast<unsigned long>(r.pc),
			static_cast<unsigned long>(r.lr), static_cast<unsigned>(r.errorCode));
		__set_PRIMASK(primask);
	}

	void recordNonFatal(uint16_t errorCode, uint32_t caller) {
		const Record *prev = saved();
		if (prev && (!announced() || prev->kind != static_cast<uint8_t>(Kind::ERROR))) return; // starší záznam zůstává
		const uint32_t frame[8] = {errorCode, 0, 0, 0, 0, caller, caller, 0x01000000};
		record(Kind::ERROR, frame, 0, errorCode);
		markAnnounced();
	}

	[[gnu::noinline]] void finish() {
		__disable_irq();
		// Sonda připojena: zastavit tady (chování na lavici); C_DEBUGEN maže jen POR nebo zápis DHCSR sondou
		if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) __BKPT(0);
		NVIC_SystemReset();
	}

	[[gnu::noinline]] void capture(Kind kind, const uint32_t *frame, uint32_t excReturn, uint16_t errorCode) {
		record(kind, frame, excReturn, errorCode);
		finish();
	}

	[[gnu::noinline]] void testSpin() {
		while (true) __asm volatile("");
	}

	[[gnu::noinline]] void testSpinNoIrq() {
		__disable_irq();
		while (true) __asm volatile("");
	}

	// ── Textový výpis (banner, dump) ─────────────────────────────────────

	struct BitName {
		uint8_t		bit;
		const char *name;
	};

	static size_t appendNames(char *buf, size_t size, size_t len, uint32_t value, const BitName *names, size_t n) {
		for (size_t i = 0; i < n; ++i) {
			if (!(value & (1u << names[i].bit)) || len + 1 >= size) continue;
			int w = snprintf(buf + len, size - len, " %s", names[i].name);
			if (w < 0) break;
			len = (len + static_cast<size_t>(w) < size) ? len + static_cast<size_t>(w) : size - 1;
		}
		return len;
	}

	static const BitName CFSR_NAMES[] = {
		{0, "IACCVIOL"},
		{1, "DACCVIOL"},
		{3, "MUNSTKERR"},
		{4, "MSTKERR"},
		{5, "MLSPERR"},
		{7, "MMARVALID"},
		{8, "IBUSERR"},
		{9, "PRECISERR"},
		{10, "IMPRECISERR"},
		{11, "UNSTKERR"},
		{12, "STKERR"},
		{13, "LSPERR"},
		{15, "BFARVALID"},
		{16, "UNDEFINSTR"},
		{17, "INVSTATE"},
		{18, "INVPC"},
		{19, "NOCP"},
		{24, "UNALIGNED"},
		{25, "DIVBYZERO"},
	};
	static const BitName HFSR_NAMES[] = {{1, "VECTTBL"}, {30, "FORCED"}, {31, "DEBUGEVT"}};

	size_t formatLine(const Record &r, uint8_t idx, char *buf, size_t size) {
		using UL = unsigned long;
		int n	 = 0;
		switch (idx) {
			case 0:
				n = snprintf(buf, size, "%s #%u code %u up %lu.%03lu s rev %.7s", kindName(r.kind), static_cast<unsigned>(r.count),
					static_cast<unsigned>(r.errorCode), static_cast<UL>(r.uptimeMs / 1000), static_cast<UL>(r.uptimeMs % 1000), r.rev);
				break;
			case 1:
				n = snprintf(buf, size, "pc=0x%08lx lr=0x%08lx xpsr=0x%08lx exc_return=0x%08lx ipsr=%lu", static_cast<UL>(r.pc), static_cast<UL>(r.lr),
					static_cast<UL>(r.xpsr), static_cast<UL>(r.excReturn), static_cast<UL>(r.ipsr));
				break;
			case 2:
				n = snprintf(buf, size, "r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx r12=0x%08lx", static_cast<UL>(r.r0), static_cast<UL>(r.r1),
					static_cast<UL>(r.r2), static_cast<UL>(r.r3), static_cast<UL>(r.r12));
				break;
			case 3: {
				n = snprintf(buf, size, "cfsr=0x%08lx", static_cast<UL>(r.cfsr));
				if (n < 0) return 0;
				size_t len = static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
				return appendNames(buf, size, len, r.cfsr, CFSR_NAMES, sizeof(CFSR_NAMES) / sizeof(CFSR_NAMES[0]));
			}
			case 4: {
				n = snprintf(buf, size, "hfsr=0x%08lx", static_cast<UL>(r.hfsr));
				if (n < 0) return 0;
				size_t len = static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
				return appendNames(buf, size, len, r.hfsr, HFSR_NAMES, sizeof(HFSR_NAMES) / sizeof(HFSR_NAMES[0]));
			}
			case 5:
				n = snprintf(buf, size, "mmfar=0x%08lx bfar=0x%08lx afsr=0x%08lx msp=0x%08lx psp=0x%08lx", static_cast<UL>(r.mmfar), static_cast<UL>(r.bfar),
					static_cast<UL>(r.afsr), static_cast<UL>(r.msp), static_cast<UL>(r.psp));
				break;
			default: return 0;
		}
		if (n <= 0) return 0;
		return static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
	}

} // namespace fault

// ── Vstupy výjimek ───────────────────────────────────────────────────────
// Naked stub: aktivní zásobník podle EXC_RETURN bit 2 (0 = MSP, 1 = PSP) → r0 = uložený rámec, r1 = EXC_RETURN,
// r2 = druh; pak skok do C++ (nevrací se). Rámec s FPU (EXC_RETURN bit 4 = 0) začíná stejnými 8 slovy.

extern "C" [[gnu::used, gnu::externally_visible, noreturn]] void fault_entry(const uint32_t *frame, uint32_t excReturn, uint32_t kind) {
	fault::capture(static_cast<fault::Kind>(kind), frame, excReturn);
}

#define FAULT_STUB(handler, kind) \
	extern "C" [[gnu::naked]] void handler(void) { \
		__asm volatile( \
			"tst lr, #4\n\t" \
			"ite eq\n\t" \
			"mrseq r0, msp\n\t" \
			"mrsne r0, psp\n\t" \
			"mov r1, lr\n\t" \
			"movs r2, #" #kind \
			"\n\t" \
			"b fault_entry\n\t"); \
	}

FAULT_STUB(HardFault_Handler, 1)
FAULT_STUB(MemManage_Handler, 2)
FAULT_STUB(BusFault_Handler, 3)
FAULT_STUB(UsageFault_Handler, 4)
// NMI (např. CSS na HSE): jako HARDFAULT, rozliší ho ipsr = 2
FAULT_STUB(NMI_Handler, 1)
