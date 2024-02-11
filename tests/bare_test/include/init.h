.altmacro

.macro FILL_NOOP_IDT
LOCAL fill
// fill up noop handlers
	xorw	%ax, %ax
	xorw	%di, %di
	movw	%ax, %es
	movw	$256, %cx
fill:
	movw	$noop_handler, %es:(%di)
	movw	%cs, %es:2(%di)
	add	$4, %di
	loop	fill
.endm

.macro PRINT str_begin, str_end
	mov	$0x3f8,%dx
	cs lea	str_begin, %si
	mov	$(str_end-str_begin), %cx
	cs rep/outsb
.endm

.macro SETA20
LOCAL seta20.1, seta20.2
seta20.1:
	inb	$0x64,%al
	testb	$0x2,%al
	jnz	seta20.1 

	movb	$0xd1,%al
	outb	%al,$0x64

seta20.2:
	inb	$0x64,%al
	testb	$0x2,%al
	jnz	seta20.2

	movb	$0xdf,%al
	outb	%al,$0x60
.endm

.macro PROTECT_MODE
	lgdt	gdtdesc32
	movl	%cr0, %eax
	orl	$CR0_PE, %eax
	movl	%eax, %cr0

	ljmp	$(SEG_KCODE<<3), $start32
.endm

.macro START32
start32:
	# Set up the protected-mode data segment registers
	movw	$(SEG_KDATA<<3), %ax	# Our data segment selector
	movw	%ax, %ds				# -> DS: Data Segment
	movw	%ax, %es				# -> ES: Extra Segment
	movw	%ax, %ss				# -> SS: Stack Segment
	movw	$0, %ax					# Zero segments not ready for use
	movw	%ax, %fs				# -> FS
	movw	%ax, %gs				# -> GS
.endm

.macro SETUP_SSE
LOCAL check_sse, enable_sse
	// Check for SSE and enable it. If it's not supported, reboot
check_sse:
	movl	$0x1, %eax
	cpuid
	testl	$1<<25, %edx
	jnz	enable_sse
	REBOOT

enable_sse:
	movl	%cr0, %eax
	andw	$0xFFFB, %ax	// Clear coprocessor emulation CR0.EM
	orw	$0x2, %ax		// Set coprocessor monitoring CR0.MP
	movl	%eax, %cr0
	movl	%cr4, %eax
	orw	$3 << 9, %ax	// set CR4.OSFXSR and CR4.OSXMMEXCPT
						// at the same time
	movl	%eax, %cr4
.endm

.macro REBOOT
	/* Reboot by using the i8042 reboot line */
	mov	$0xfe, %al
	outb	%al, $0x64
.endm

.macro JUST_LOOP
LOCAL inf_loop
inf_loop:
	jmp	inf_loop
.endm

.macro CHECK_LONG_MODE
LOCAL no_long_mode, long_mode_ready
	movl	$0x80000000, %eax	// Set the A-register to 0x80000000
	cpuid				// CPU identification
	cmp	$0x80000001, %eax	// Compare A-register with 0x80000001
	jb	no_long_mode		// It is less, there is no long mode
	movl	$0x80000001, %eax	// Set the A-register to 0x80000001
	cpuid				// CPU identification
	test	$1 << 29, %edx		// Test if the LM-bit, which is bit
					//   29, is set in the D-register
	jz	no_long_mode		// They aren't, there is no long mode
	jmp	long_mode_ready
no_long_mode:
	PRINT long_mode_err_msg, long_mode_err_msg_end
	REBOOT
long_mode_ready:
.endm

.macro SET_PAGE_TABLE
LOCAL map_p2_table
	// Recursive map P4
	mov	$p4_table, %eax
	orl	$0b11, %eax		// present + writable
	movl	%eax, (p4_table + 511 * 8)

	// Map first P4 entry to P3 table
	movl	$p3_table, %eax
	orl	$0b11, %eax		// present + writable
	movl	%eax, (p4_table)

	// Map first P3 entry to P2 table
	movl	$p2_table, %eax
	orl	$0b11, %eax		// present + writable
	mov	%eax, (p3_table)

	// Map each P2 entry to a huge 2MiB page
	movl	$0, %ecx		// counter variable
map_p2_table:
	// Map ecx-th P2 entry to a huge page that starts at address
	// (2MiB * ecx)
	movl	$0x200000, %eax		// 2MiB
	mul	%ecx			// start address of ecx-th page
	orl	$0b10000011, %eax	// present + writable + huge
	movl	%eax, p2_table(,%ecx,8) // map ecx-th entry

	inc	%ecx			// increase counter
	cmp	$512, %ecx		// if counter == 512, the
					//   whole P2 table is mapped
	jne	map_p2_table		// else map the next entry
.endm

.macro ENABLE_PAGING pgtbl
	// Load P4 to cr3 register (cpu uses this to access the P4 table)
	movl	pgtbl, %eax
	movl	%eax, %cr3

	// Enable PAE-flag in cr4 (Physical Address Extension)
	movl	%cr4, %eax
	orl	$1 << 5, %eax
	mov	%eax, %cr4

	// Set the long mode bit in the EFER MSR (model specific register)
	mov	$0xC0000080, %ecx
	rdmsr
	orl	$1 << 8, %eax
	wrmsr

	// Enable paging in the cr0 register
	movl	%cr0, %eax
	orl	$1 << 31, %eax
	mov	%eax, %cr0
.endm
