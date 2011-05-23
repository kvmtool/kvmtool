#include "kvm/segment.h"
#include "kvm/bios.h"
#include "kvm/util.h"
#include "kvm/vesa.h"
#include <stdint.h>

#define VESA_MAGIC ('V' + ('E' << 8) + ('S' << 16) + ('A' << 24))

struct int10_args {
	u32	eax;
	u32	ebx;
	u32	ecx;
	u32	edx;
	u32	esp;
	u32	ebp;
	u32	esi;
	u32	edi;
	u32	es;
};

/* VESA General Information table */
struct vesa_general_info {
	u32 signature;			/* 0 Magic number = "VESA" */
	u16 version;			/* 4 */
	void *vendor_string;		/* 6 */
	u32 capabilities;		/* 10 */
	void *video_mode_ptr;		/* 14 */
	u16 total_memory;		/* 18 */

	u8 reserved[236];		/* 20 */
} __attribute__ ((packed));


struct vminfo {
	u16	mode_attr;		/* 0 */
	u8	win_attr[2];		/* 2 */
	u16	win_grain;		/* 4 */
	u16	win_size;		/* 6 */
	u16	win_seg[2];		/* 8 */
	u32	win_scheme;		/* 12 */
	u16	logical_scan;		/* 16 */

	u16	h_res;			/* 18 */
	u16	v_res;			/* 20 */
	u8	char_width;		/* 22 */
	u8	char_height;		/* 23 */
	u8	memory_planes;		/* 24 */
	u8	bpp;			/* 25 */
	u8	banks;			/* 26 */
	u8	memory_layout;		/* 27 */
	u8	bank_size;		/* 28 */
	u8	image_planes;		/* 29 */
	u8	page_function;		/* 30 */

	u8	rmask;			/* 31 */
	u8	rpos;			/* 32 */
	u8	gmask;			/* 33 */
	u8	gpos;			/* 34 */
	u8	bmask;			/* 35 */
	u8	bpos;			/* 36 */
	u8	resv_mask;		/* 37 */
	u8	resv_pos;		/* 38 */
	u8	dcm_info;		/* 39 */

	u32	lfb_ptr;		/* 40 Linear frame buffer address */
	u32	offscreen_ptr;		/* 44 Offscreen memory address */
	u16	offscreen_size;		/* 48 */

	u8	reserved[206];		/* 50 */
};

char oemstring[11] = "KVM VESA";
u16 modes[2] = { 0x0112, 0xffff };

static inline void outb(unsigned short port, unsigned char val)
{
	asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * It's probably much more useful to make this print to the serial
 * line rather than print to a non-displayed VGA memory
 */
static inline void int10_putchar(struct int10_args *args)
{
	u8 al, ah;

	al = args->eax & 0xFF;
	ah = (args->eax & 0xFF00) >> 8;

	outb(0x3f8, al);
}

static void int10_vesa(struct int10_args *args)
{
	u8 al, ah;
	struct vesa_general_info *destination;
	struct vminfo *vi;

	al = args->eax;
	ah = args->eax >> 8;

	switch (al) {
	case 0:
		/* Set controller info */

		destination = (struct vesa_general_info *)args->edi;
		*destination = (struct vesa_general_info) {
			.signature	= VESA_MAGIC,
			.version	= 0x102,
			.vendor_string	= oemstring,
			.capabilities	= 0x10,
			.video_mode_ptr	= modes,
			.total_memory	= (4*VESA_WIDTH * VESA_HEIGHT) / 0x10000,
		};

		break;
	case 1:
		vi = (struct vminfo *)args->edi;
		*vi = (struct vminfo) {
			.mode_attr	= 0xd9, /* 11011011 */
			.logical_scan	= VESA_WIDTH*4,
			.h_res		= VESA_WIDTH,
			.v_res		= VESA_HEIGHT,
			.bpp		= VESA_BPP,
			.memory_layout	= 6,
			.memory_planes	= 1,
			.lfb_ptr	= VESA_MEM_ADDR,
			.rmask		= 8,
			.gmask		= 8,
			.bmask		= 8,
			.resv_mask	= 8,
			.resv_pos	= 24,
			.bpos		= 16,
			.gpos		= 8,
		};

		break;
	}

	args->eax			= 0x004f; /* return success every time */

}

bioscall void int10_handler(struct int10_args *args)
{
	u8 ah;

	ah = (args->eax & 0xff00) >> 8;

	switch (ah) {
	case 0x0e:
		int10_putchar(args);
		break;
	case 0x4f:
		int10_vesa(args);
		break;
	}

}
