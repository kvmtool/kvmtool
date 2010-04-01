#ifndef BIOS_H_
#define BIOS_H_

/*
 * BIOS data area
 */
#define BDA_START		0xf0000
#define BDA_END			0xfffff
#define BDA_SIZE		(BDA_END - BDA_START)

#define VIDEO_BASE		0xa0000
#define VIDEO_BASE_SEG		0xa000
#define VIDEO_LIMIT		0xbffff
#define VIDEO_SIZE		0x007d0 /* 80x25 */

#define REAL_INTR_BASE		0x0000
#define REAL_INTR_VECTORS	256

#define BIOS_STACK		0xFFF70
#define BIOS_STACK_SEG		0xFFF7
#define BIOS_STACK		128

#define ALIGN(x, a)		(((x)+((a)-1))&~((a)-1))

#define BIOS_INTR_NEXT(prev, size)	\
	((prev) + ALIGN(size, 16))

#endif /* BIOS_H_ */
