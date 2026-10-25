// Glitch routine for the bootloader tests (bootfi.py loads it into SRAM via the probe, runs on G071 and G431: Thumb-1).
// Starts a real page erase / double-word program and resets the MCU (SYSRESETREQ) after `delay` iterations, i.e.
// interrupts the flash operation midway - the closest substitute for a power loss that the bench can do.
// op: 0 = erase + reset, 1 = program + reset, 2 = erase + measure (iterations until BSY ends, bkpt), 3 = program + measure,
// 4 = erase the page (let it finish) and then program + reset, 5 = erase the page and then program + measure. 4/5 run from the
// application state: programming from a halted base bootloader reports PGAERR|PGSERR (bench 2026-10-24, cause unknown).
#include <stdint.h>

typedef struct {
	uint32_t flash; // FLASH base (0x40022000 on both)
	uint32_t op;
	uint32_t addr;  // page / double-word address
	uint32_t delay; // iterations between the operation start and the reset
	uint32_t w0, w1;
	uint32_t page;  // page number for PNB
	uint32_t result; // op 2/3: iterations until the operation ends; 0xE0000000 | SR on error
} Params;

#define P ((volatile Params *)0x20000F00u)
#define KEYR 2u // offsets in words
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
	f[SR] = 0x0000C3FBu; // clear the error flags
	uint32_t op = P->op;
	if (op >= 4) {
		f[CR] = (P->page << 3) | (1u << 1);
		f[CR] |= (1u << 16);
		while (f[SR] & BSY) {}
		f[CR] = 0;
		f[SR] = 0x0000C3FBu;
		op -= 3; // 4 -> 1 (program + reset), 5 -> 2 ... see below: program + measure
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
		return; // bkpt in entry
	}
	// Same body as the measuring loop above (one SR read, test, increment), so that `delay` from the measurement matches the time:
	// the original loop with a volatile counter was ~1.5x slower (review 2026-10-25), so the late part of the window was never sampled.
	uint32_t d = P->delay;
	uint32_t i = 0;
	while (i < d) {
		if (f[SR] & 0x80000000u) break; // bit 31 of SR is always 0: just an SR read as in the measurement
		i++;
	}
	*(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u; // AIRCR.SYSRESETREQ
	for (;;) {}
}
