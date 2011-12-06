/*
 * Local variables for almost every BIOS irq handler
 * Must be put somewhere inside irq handler body
 */
__CALLER_SS:		.int  0
__CALLER_SP:		.long 0
__CALLER_CLOBBER:	.long 0
