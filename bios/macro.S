/*
 * handy BIOS macros
 */

/*
 * switch to BIOS stack
 */
.macro stack_swap
	mov %ss, %cs:(__CALLER_SS)
	mov %sp, %cs:(__CALLER_SP)
	mov %dx, %cs:(__CALLER_CLOBBER)
	mov $MB_BIOS_SS, %dx
	mov %dx, %ss
	mov $MB_BIOS_SP, %sp
	mov %cs:(__CALLER_CLOBBER), %dx
.endm

/*
 * restore the original stack
 */
.macro stack_restore
	mov %cs:(__CALLER_SP), %sp
	mov %cs:(__CALLER_SS), %ss
.endm

