/*
 * IRQ 0x10 handler - output in video memory
 */

#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
	.code16gcc

#include "macro.S"

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
	test $0x0e, %ah
	jne out

/*
 * put char in AL at current cursor and
 * increment cursor position
 */
putchar:
	stack_swap

	push %fs
	push %bx

	mov $VGA_RAM_SEG, %bx
	mov %bx, %fs
	mov %cs:(cursor), %bx
	mov %al, %fs:(%bx)
	inc %bx
	test $VGA_PAGE_SIZE, %bx
	jb putchar_new
	xor %bx, %bx
putchar_new:
	mov %bx, %fs:(cursor)

	pop %bx
	pop %fs

	stack_restore
out:
	sti
	IRET
/*
 * private IRQ data
 */
cursor:		.long 0

/*
 * must be last in this file
 */
#include "local.S"
ENTRY_END(___int10_end)
