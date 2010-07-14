#ifndef BIOS_H_
#define BIOS_H_

/*
 * X86-32 Memory Map (typical)
 *					start      end
 * Real Mode Interrupt Vector Table	0x00000000 0x000003FF
 * BDA area				0x00000400 0x000004FF
 * Conventional Low Memory		0x00000500 0x0009FBFF
 * EBDA area				0x0009FC00 0x0009FFFF
 * VIDEO RAM				0x000A0000 0x000BFFFF
 * VIDEO ROM (BIOS)			0x000C0000 0x000C7FFF
 * Motherboard BIOS			0x000F0000 0x000FFFFF
 * Extended Memory			0x00100000 0xFEBFFFFF
 * Reserved (configs, ACPI, PnP, etc)	0xFEC00000 0xFFFFFFFF
 */

#define REAL_MODE_IVT_BEGIN		0x00000000
#define REAL_MODE_IVT_END		0x000003ff

#define BDA_START			0x00000400
#define BDA_END				0x000004ff

#define EBDA_START			0x0009fc00
#define EBDA_END			0x0009ffff

#define E820_MAP_SIZE			EBDA_START
#define E820_MAP_START			(EBDA_START + 0x01)

#define MB_BIOS_BEGIN			0x000f0000
#define MB_BIOS_END			0x000fffff

#define VGA_RAM_BEGIN			0x000a0000
#define VGA_RAM_END			0x000bffff

#define VGA_ROM_BEGIN			0x000c0000
#define VGA_ROM_END			0x000c7fff

/* we handle one page only */
#define VGA_RAM_SEG			(VGA_RAM_BEGIN >> 4)
#define VGA_PAGE_SIZE			0x007d0 /* 80x25 */

/* real mode interrupt vector table */
#define REAL_INTR_BASE			REAL_MODE_IVT_BEGIN
#define REAL_INTR_VECTORS		256

/*
 * BIOS stack must be at absolute predefined memory address
 * We reserve 64 bytes for BIOS stack
 */
#define MB_BIOS_SS			0xfff7
#define MB_BIOS_SP			0x40

#define ALIGN(x, a)	\
	(((x) + ((a) - 1)) & ~((a) - 1))

/*
 * note we use 16 bytes alignment which makes segment based
 * addressing easy to compute, dont change it otherwise you
 * may break local variables offsets in BIOS irq routines
 */
#define BIOS_NEXT_IRQ_ADDR(addr, size)	\
	ALIGN((addr + size + 1), 16)

#endif /* BIOS_H_ */
