/*
 * IRQ 0x15 handler - e820 memory map
 */

#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
	.code16gcc

#include "macro.S"

ENTRY(___int15)
	jmp out

#	movl %cs, %eax
#	movl %eax, %es
#	movl $0x534D4150, %eax	# 'SMAP'
#	movl $0, %ebx		# end of map
#	movl $20, %ecx
#	mov e820entry_start, %di
#	mov $(e820entry_end - e820entry_start), %ecx

out:
	sti
	IRET
/*
 * private IRQ data
 */
GLOBAL(e820entry_start)
	e820entry_addr:	.quad 0
	e820entry_size:	.quad 0
	e820entry_type:	.long 0
GLOBAL(e820entry_end)

/*
 * must be last in this file
 */
#include "local.S"
ENTRY_END(___int15_end)
