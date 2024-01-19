#include <mmu.h>
#include <traps.h>
#include <interrupt.h>
#include <stdio.h>
#include <pit.h>
#include <x86.h>

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint64_t vectors[];  // in vectors.S: array of 256 entry pointers

#define PLANED_TICK_NUM 1000
static int ticks = 0;

void tvinit(void) {
	int i;

	for(i = 0; i < 256; i++)
		SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
	SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
}

void idtinit(void) {
	lidt(idt, sizeof(idt));
}

static void syscall(void) { }

//PAGEBREAK: 41
void trap(struct trapframe *tf) {
	if (tf->trapno == T_SYSCALL) {
		syscall();
		return;
	}

	switch (tf->trapno) {
	case T_IRQ0 + IRQ_TIMER:
		if (++ticks % PLANED_TICK_NUM == 0) {
			kprintf("%u ticks\n", ticks);
			if (++ticks / PLANED_TICK_NUM == 10){
				reboot();
			}
		}
		break;
	default:
		kprintf("unexpected trap %u from rip %x\n", tf->trapno, tf->rip);
	}
}
