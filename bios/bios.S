/*
 * Our pretty trivial BIOS emulation
 */

#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
	.code16gcc

#include "macro.S"

#define EFLAGS_CF	(1 << 0)

/*
 * fake interrupt handler, nothing can be faster ever
 */
ENTRY(bios_intfake)
	/*
	 * Set CF to indicate failure. We don't want callers to think that the
	 * interrupt handler succeeded and then treat the return values in
	 * registers as valid data.
	 */
	orl	$EFLAGS_CF, 0x4(%esp)

	IRET
ENTRY_END(bios_intfake)

/*
 * int 10 - video - service
 */
ENTRY(bios_int10)
	pushw	%fs
	pushl	%es
	pushl	%edi
	pushl	%esi
	pushl	%ebp
	pushl	%esp
	pushl	%edx
	pushl	%ecx
	pushl	%ebx
	pushl	%eax

	movl		%esp, %eax
	/* this is way easier than doing it in assembly */
	/* just push all the regs and jump to a C handler */
	call	int10_handler

	popl	%eax
	popl	%ebx
	popl	%ecx
	popl	%edx
	popl	%esp
	popl	%ebp
	popl	%esi
	popl	%edi
	popl	%es
	popw	%fs

	/* Clear CF to indicate success.  */
	andl	$~EFLAGS_CF, 0x4(%esp)

	IRET
ENTRY_END(bios_int10)

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

	/* Clear CF to indicate success.  */
	andl	$~EFLAGS_CF, 0x4(%esp)
1:
	IRET
ENTRY_END(bios_int15)

GLOBAL(__locals)

#include "local.S"

END(__locals)
