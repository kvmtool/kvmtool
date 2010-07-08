/*
 * IRQ 0x15 handler - e820 memory map
 */

#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
	.code16gcc

#include "macro.S"

#define EFLAGS_CF	(1 << 0)

ENTRY(___int15)
	cmp $0xE820, %eax
	jne out

	pushw	%fs

	pushl	%edx
	pushl	%ecx
	pushl	%edi
	pushl	%ebx
	pushl	%eax

	xor	%ax, %ax
	mov	%ax, %fs

	movl	%esp, %eax
	call	e820_query_map

	popl	%eax
	popl	%ebx
	popl	%edi
	popl	%ecx
	popl	%edx

	popw	%fs

	/* Clear CF */
	andl	$~EFLAGS_CF, 0x4(%esp)
out:
	sti
	IRET

/*
 * must be last in this file
 */
#include "local.S"
ENTRY_END(___int15_end)
