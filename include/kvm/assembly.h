#ifndef ASSEMBLY_H_
#define ASSEMBLY_H_

#define __ALIGN	.p2align 4, 0x90
#define ENTRY(name)	\
	__ALIGN;	\
	.globl name;	\
	name:

#define ENTRY_END(name)

/*
 * gas produces size override prefix with which
 * we are unhappy, lets make it hardcoded for
 * 16 bit mode
 */
#define IRET	.byte 0xcf

#endif /* ASSEMBLY_H_ */
