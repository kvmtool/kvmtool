#ifndef KVM__CPU_H
#define KVM__CPU_H

#include <stdint.h>

enum eflag_bits {
	EFLAGS_CF		= (1UL << 0),	/* Carry Flag */
	EFLAGS_PF		= (1UL << 2),	/* Parity Flag */
	EFLAGS_AF		= (1UL << 4),	/* Auxiliary Carry Flag */
	EFLAGS_ZF		= (1UL << 6),	/* Zero Flag */
	EFLAGS_SF		= (1UL << 7),	/* Sign Flag */
	EFLAGS_TF		= (1UL << 8),	/* Trap Flag */
	EFLAGS_IF		= (1UL << 9),	/* Interrupt Enable Flag */
	EFLAGS_DF		= (1UL << 10),	/* Direction Flag */
	EFLAGS_OF		= (1UL << 11),	/* Overflow Flag */
	EFLAGS_NT		= (1UL << 14),	/* Nested Task */
	EFLAGS_RF		= (1UL << 16),	/* Resume Flag */
	EFLAGS_VM		= (1UL << 17),	/* Virtual-8086 Mode */
	EFLAGS_AC		= (1UL << 18),	/* Alignment Check */
	EFLAGS_VIF		= (1UL << 19),	/* Virtual Interrupt Flag */
	EFLAGS_VIP		= (1UL << 20),	/* Virtual Interrupt Pending */
	EFLAGS_ID		= (1UL << 21),	/* ID Flag */
};

struct cpu_registers {
	uint32_t		eax;
	uint32_t		ebx;
	uint32_t		ecx;
	uint32_t		edx;
	uint32_t		esp;
	uint32_t		ebp;
	uint32_t		esi;
	uint32_t		edi;
	uint32_t		eip;
	uint32_t		eflags;
};

struct cpu {
	struct cpu_registers	regs;	
};

struct cpu *cpu__new(void);
void cpu__reset(struct cpu *self);

#endif /* KVM__CPU_H */
