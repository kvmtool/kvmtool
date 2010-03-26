	.code16gcc
	.text
	.globl  _start
	.type   _start, @function
_start:
	# "This is probably the largest possible kernel that is bug free." -- Avi Kivity
	1:
	jmp 1b
