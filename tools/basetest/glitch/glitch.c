// Glitch rutina pro testy bootloaderu (nahrava ji bootfi.py sondou do SRAM, bezi na G071 i G431: Thumb-1).
// Spusti skutecne mazani stranky / programovani dvojslova a po `delay` iteracich resetuje MCU (SYSRESETREQ), tj.
// preruseni flash operace v jejim prubehu - nejblizsi nahrada vypadku napajeni, kterou lavice umi.
// op: 0 = erase + reset, 1 = program + reset, 2 = erase + mereni (iterace do konce BSY, bkpt), 3 = program + mereni,
// 4 = smazat stranku (dokoncit) a pak program + reset, 5 = smazat stranku a pak program + mereni. 4/5 bezi ze stavu
// aplikace: programovani ze zastaveneho bootloaderu base hlasi PGAERR|PGSERR (bench 2026-10-24, pricina neznama).
#include <stdint.h>

typedef struct {
	uint32_t flash; // FLASH base (0x40022000 na obou)
	uint32_t op;
	uint32_t addr;  // adresa stranky / dvojslova
	uint32_t delay; // iterace mezi startem operace a resetem
	uint32_t w0, w1;
	uint32_t page;  // cislo stranky pro PNB
	uint32_t result; // op 2/3: iterace do konce operace; 0xE0000000 | SR pri chybe
} Params;

#define P ((volatile Params *)0x20000F00u)
#define KEYR 2u // offsety ve slovech
#define SR 4u
#define CR 5u
#define BSY (1u << 16)

void run(void);

__attribute__((naked, section(".entry"))) void entry(void) {
	__asm volatile("cpsid i\n"
				   "ldr r0, =0x20002000\n"
				   "mov sp, r0\n"
				   "bl run\n"
				   "bkpt #0\n");
}

void run(void) {
	volatile uint32_t *f = (volatile uint32_t *)P->flash;
	if (f[CR] & (1u << 31)) {
		f[KEYR] = 0x45670123u;
		f[KEYR] = 0xCDEF89ABu;
	}
	while (f[SR] & BSY) {}
	f[SR] = 0x0000C3FBu; // smazat chybove priznaky
	uint32_t op = P->op;
	if (op >= 4) {
		f[CR] = (P->page << 3) | (1u << 1);
		f[CR] |= (1u << 16);
		while (f[SR] & BSY) {}
		f[CR] = 0;
		f[SR] = 0x0000C3FBu;
		op -= 3; // 4 -> 1 (program + reset), 5 -> 2 ... viz nize: program + mereni
		if (op == 2) op = 3;
	}
	if (op == 0 || op == 2) {
		f[CR] = (P->page << 3) | (1u << 1); // PNB | PER
		f[CR] |= (1u << 16);                // STRT
	} else {
		f[CR] = 1u; // PG
		*(volatile uint32_t *)P->addr = P->w0;
		*(volatile uint32_t *)(P->addr + 4) = P->w1;
	}
	if (op >= 2) {
		uint32_t n = 0;
		while (f[SR] & BSY) n++;
		f[CR] = 0;
		uint32_t sr = f[SR] & 0x0000C3FAu;
		P->result = sr ? (0xE0000000u | sr) : n;
		return; // bkpt v entry
	}
	// Stejne telo jako merici smycka nahore (jedno cteni SR, test, inkrement), aby `delay` z mereni odpovidal case:
	// puvodni smycka s volatile citacem byla ~1,5x pomalejsi (review 2026-10-25), pozdni cast okna se nevzorkovala.
	uint32_t d = P->delay;
	uint32_t i = 0;
	while (i < d) {
		if (f[SR] & 0x80000000u) break; // bit 31 SR je vzdy 0: jen cteni SR jako v mereni
		i++;
	}
	*(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u; // AIRCR.SYSRESETREQ
	for (;;) {}
}
