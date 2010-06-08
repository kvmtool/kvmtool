#define IO_PIC		0x20
#define IRQ_OFFSET	32
#define IO_PIT		0x40
#define TIMER_FREQ	1193182
#define TIMER_DIV(x)	((TIMER_FREQ+(x)/2)/(x))

	.code16gcc
	.text
	.globl	_start
	.type	_start, @function
_start:
	mov	$0x3f8,%dx
	cs lea	msg, %si
	mov	$(msg_end-msg), %cx
	cs rep/outsb

set_idt:
	xor	%ax, %ax
	movw	%ax, %es
	movw	$timer_isr, %es:(IRQ_OFFSET*4)
	movw	%cs, %es:(IRQ_OFFSET*4+2)

set_pic:
	# ICW1
	mov	$0x11, %al
	mov	$(IO_PIC), %dx
	out	%al,%dx
	# ICW2
	mov	$(IRQ_OFFSET), %al
	mov	$(IO_PIC+1), %dx
	out	%al, %dx
	# ICW3
	mov	$0x00, %al
	mov	$(IO_PIC+1), %dx
	out	%al, %dx
	# ICW4
	mov	$0x3, %al
	mov	$(IO_PIC+1), %dx
	out	%al, %dx

set_pit:
	# set 8254 mode
	mov	$(IO_PIT+3), %dx
	mov	$0x34, %al
	outb	%al, %dx
	# set 8254 freq 100Hz
	mov	$(IO_PIT), %dx
	movb	$(TIMER_DIV(100) % 256), %al
	outb	%al, %dx
	movb	$(TIMER_DIV(100) / 256), %al
	outb	%al, %dx

enable_irq0:
	mov	$0xfe, %al
	mov	$(IO_PIC+1), %dx
	out	%al, %dx
	sti
loop:
	1:
	jmp	1b

timer_isr:
	pushaw
	mov	$0x3f8,%dx
	mov	$0x49, %al	# I
	out	%al,%dx
	mov	$0x53, %al	# S
	out	%al,%dx
	mov	$0x52, %al	# R
	out	%al,%dx
	mov	$0x0a, %al	# \n
	out	%al, %dx
	popaw
	iretw

msg:
	.ascii "This is PVM.\nPIC 8259 and PIT 8254 test.\n"
	.asciz "---------------------------------------------------\n"
msg_end:
