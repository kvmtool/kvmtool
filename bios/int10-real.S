/*
 * IRQ 0x10 handler
 */

#include <kvm/bios.h>
#include <kvm/assembly.h>

/*
 * NOTES:
 * ======
 * 1) The code is supposed to enter with CS somewhere in BDA_START area so prepare real mode
 *    interrupt table properly (if you screw it up -- there is no way to restore
 *    anything, the code is NOT PORTABLE by any means, it's BIOS after all)
 *
 * 2) We switch to own stack which is BIOS_STACK_SIZE bytes long saving all caller's
 *    data we clobber. The stack is not that deep so mask interrupts at entry and dont
 *    use 'push' too much
 */

	.org 0
	.code16gcc

.macro stack_bios seg
	mov %ss, %\seg:(ss_old)
	mov %sp, %\seg:(sp_old)
	mov %si, %\seg:(stack_clobber)
	mov $BIOS_STACK_SEG, %si
	mov %si, %ss
	mov $BIOS_STACK_SIZE, %sp
	mov %\seg:(stack_clobber), %si
.endm

.macro stack_norm seg
	mov %\seg:(ss_old), %ss
	mov %\seg:(sp_old), %sp
.endm

.macro opcode_switch ocode, label
	test \label, %ah
	je \label
.endm

/*
 * int 10 - video - write character and advance cursor (tty write)
 *	ah = 0eh
 *	al = character
 *	bh = display page (alpha modes)
 *	bl = foreground color (graphics modes)
 *
 * We ignore bx settings
 */
ENTRY(___int10)
	cli
	opcode_switch $0x0e, putchar
	jmp out

/*
 * put char in AL at current cursor and
 * increment cursor position
 */
putchar:
	stack_bios cs

	push %fs
	push %bx

	mov $VIDEO_BASE_SEG, %bx
	mov %bx, %fs
	mov %cs:(cursor), %bx
	mov %al, %fs:(%bx)
	inc %bx
	test $VIDEO_SIZE, %bx
	jb putchar_new
	xor %bx, %bx
putchar_new:
	mov %bx, %fs:(cursor)

	pop %bx
	pop %fs

	stack_norm cs
out:
	sti
	IRET
/*
 * private IRQ data
 */
cursor:		.word 0

/*
 * must be last in this file
 */
	__ALIGN
ss_old:		.word 0
sp_old:		.word 0
stack_clobber:	.word 0
ENTRY_END(___int10_end)
