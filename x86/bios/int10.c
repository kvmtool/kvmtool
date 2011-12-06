#include "kvm/segment.h"
#include "kvm/bios.h"
#include "kvm/vesa.h"

#include "bios/memcpy.h"

#include <boot/vesa.h>

static far_ptr gen_far_ptr(unsigned int pa)
{
	far_ptr ptr;

	ptr.seg = (pa >> 4);
	ptr.off = pa - (ptr.seg << 4);

	return ptr;
}

static inline void outb(unsigned short port, unsigned char val)
{
	asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * It's probably much more useful to make this print to the serial
 * line rather than print to a non-displayed VGA memory
 */
static inline void int10_putchar(struct biosregs *args)
{
	u8 al = args->eax & 0xFF;

	outb(0x3f8, al);
}

static void vbe_get_mode(struct biosregs *args)
{
	struct vesa_mode_info *info = (struct vesa_mode_info *) args->edi;

	*info = (struct vesa_mode_info) {
		.mode_attr		= 0xd9, /* 11011011 */
		.logical_scan		= VESA_WIDTH*4,
		.h_res			= VESA_WIDTH,
		.v_res			= VESA_HEIGHT,
		.bpp			= VESA_BPP,
		.memory_layout		= 6,
		.memory_planes		= 1,
		.lfb_ptr		= VESA_MEM_ADDR,
		.rmask			= 8,
		.gmask			= 8,
		.bmask			= 8,
		.resv_mask		= 8,
		.resv_pos		= 24,
		.bpos			= 16,
		.gpos			= 8,
	};
}

static void vbe_get_info(struct biosregs *args)
{
	struct vesa_general_info *infop = (struct vesa_general_info *) args->edi;
	struct vesa_general_info info;

	info = (struct vesa_general_info) {
		.signature		= VESA_MAGIC,
		.version		= 0x102,
		.vendor_string		= gen_far_ptr(VGA_ROM_BEGIN),
		.capabilities		= 0x10,
		.video_mode_ptr		= gen_far_ptr(VGA_ROM_MODES),
		.total_memory		= (4 * VESA_WIDTH * VESA_HEIGHT) / 0x10000,
	};

	memcpy16(args->es, infop, args->ds, &info, sizeof(info));
}

#define VBE_STATUS_OK		0x004F

static void int10_vesa(struct biosregs *args)
{
	u8 al;

	al = args->eax & 0xff;

	switch (al) {
	case 0x00:
		vbe_get_info(args);
		break;
	case 0x01:
		vbe_get_mode(args);
		break;
	}

	args->eax = VBE_STATUS_OK;
}

bioscall void int10_handler(struct biosregs *args)
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
