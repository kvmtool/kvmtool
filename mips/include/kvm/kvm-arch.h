#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#include <linux/sizes.h>

/*
 * Guest memory map is:
 *   0x00000000-0x0fffffff : System RAM
 *   0x10000000-0x1fffffff : I/O (defined by KVM_MMIO_START and KVM_MMIO_SIZE)
 *   0x20000000-    ...    : System RAM
 * See also kvm__init_ram().
 */

#define KVM_MMIO_START		0x10000000
#define KVM_PCI_CFG_AREA	KVM_MMIO_START
#define KVM_PCI_MMIO_AREA	(KVM_MMIO_START + 0x1000000)
#define KVM_VIRTIO_MMIO_AREA	(KVM_MMIO_START + 0x2000000)
#define KVM_MMIO_SIZE		0x10000000

/*
 * Just for reference. This and the above corresponds to what's used
 * in mipsvz_page_fault() in kvm_mipsvz.c of the host kernel.
 */
#define KVM_MIPS_IOPORT_AREA	0x1e000000
#define KVM_MIPS_IOPORT_SIZE	0x00010000
#define KVM_MIPS_IRQCHIP_AREA	0x1e010000
#define KVM_MIPS_IRQCHIP_SIZE	0x00010000

#define KVM_IRQ_OFFSET		1

/*
 * MIPS-VZ (trap and emulate is 0)
 */
#define KVM_VM_TYPE		1

#define KVM_IOEVENTFD_HAS_PIO	0

#define MAX_PAGE_SIZE		SZ_64K

#include <stdbool.h>

#include "linux/types.h"

struct kvm_arch {
	u64 entry_point;
	u64 argc;
	u64 argv;
	bool is64bit;
};

#endif /* KVM__KVM_ARCH_H */
