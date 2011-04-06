/*
 * Our pretty trivial BIOS emulation
 */

#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
	.code16gcc

#include "macro.S"

/*
 * fake interrupt handler, nothing can be faster ever
 */
ENTRY(bios_intfake)
	IRET
ENTRY_END(bios_intfake)

/*
 * int 10 - video - write character and advance cursor (tty write)
 *	ah = 0eh
 *	al = character
 *	bh = display page (alpha modes)
 *	bl = foreground color (graphics modes)
 *
 * We ignore bx settings
 */
ENTRY(bios_int10)
	test $0x0e, %ah
	jne 1f

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
1:
	IRET
/*
 * private IRQ data
 */
cursor:		.long 0
ENTRY_END(bios_int10)

#define EFLAGS_CF	(1 << 0)

ENTRY(bios_int15)
	cmp $0xE820, %eax
	jne 1f

	pushw	%fs

	pushl	%edx
	pushl	%ecx
	pushl	%edi
	pushl	%ebx
	pushl	%eax

	movl	%esp, %eax	# it's bioscall case
	call	e820_query_map

	popl	%eax
	popl	%ebx
	popl	%edi
	popl	%ecx
	popl	%edx

	popw	%fs

	/* Clear CF */
	andl	$~EFLAGS_CF, 0x4(%esp)
1:
	IRET
ENTRY_END(bios_int15)

GLOBAL(__locals)

#include "local.S"

END(__locals)
