// Glitch rutina pro testy bootloaderu (nahrava ji bootfi.py sondou do SRAM, bezi na G071 i G431: Thumb-1).
// Spusti skutecne mazani stranky / programovani dvojslova a po `delay` iteracich resetuje MCU (SYSRESETREQ), tj.
// preruseni flash operace v jejim prubehu - nejblizsi nahrada vypadku napajeni, kterou lavice umi.
// op: 0 = erase + reset, 1 = program + reset, 2 = erase + mereni (iterace do konce BSY, bkpt), 3 = program + mereni.
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
	for (volatile uint32_t i = 0; i < P->delay; i++) {}
	*(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u; // AIRCR.SYSRESETREQ
	for (;;) {}
}
