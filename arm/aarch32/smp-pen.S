#include "kvm/kvm-arch.h"

#include "arm-common/gic.h"

#define AARCH32_SMP_BAD_MAGIC	0xdeadc0de

	.arm

	.globl	smp_pen_start
	.globl	smp_jump_addr
	.globl	smp_pen_end

	.align
smp_pen_start:
	@ Ensure that the CPU interface is enabled for the wfi wakeup
	ldr	r0, =ARM_GIC_CPUI_BASE
	mov	r1, #GIC_CPUI_CTLR_EN
	str	r1, [r0]

	@ Set the priority mask to accept any interrupt
	mov	r1, #GIC_CPUI_PMR_MIN_PRIO
	str	r1, [r0, #GIC_CPUI_OFF_PMR]

	@ Now wait for the primary to poke us
	adr	r0, smp_jump_addr
	ldr	r1, =AARCH32_SMP_BAD_MAGIC
	dsb
1:	wfi
	ldr	r2, [r0]
	cmp	r1, r2
	beq	1b
	mov	pc, r2

	.ltorg

	.align
smp_jump_addr:
	.long	AARCH32_SMP_BAD_MAGIC
smp_pen_end:
