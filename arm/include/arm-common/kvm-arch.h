#ifndef ARM_COMMON__KVM_ARCH_H
#define ARM_COMMON__KVM_ARCH_H

#include <stdbool.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>

#include <linux/const.h>
#include <linux/types.h>

#include "arm-common/gic.h"

/*
 * The memory map used for ARM guests (not to scale):
 *
 * 0      64K  16M     32M     48M            1GB       2GB
 * +-------+----+-------+-------+--------+-----+---------+---......
 * |  PCI  |////| plat  |       |        |     |         |
 * |  I/O  |////| MMIO: | Flash | virtio | GIC |   PCI   |  DRAM
 * | space |////| UART, |       |  MMIO  |     |  (AXI)  |
 * |       |////| RTC,  |       |        |     |         |
 * |       |////| PVTIME|       |        |     |         |
 * +-------+----+-------+-------+--------+-----+---------+---......
 */

#define ARM_IOPORT_AREA		_AC(0x0000000000000000, UL)
#define ARM_MMIO_AREA		_AC(0x0000000001000000, UL)
#define ARM_AXI_AREA		_AC(0x0000000040000000, UL)
#define ARM_MEMORY_AREA		_AC(0x0000000080000000, UL)

#define KVM_IOPORT_AREA		ARM_IOPORT_AREA
#define ARM_IOPORT_SIZE		(1U << 16)


#define ARM_UART_MMIO_BASE	ARM_MMIO_AREA
#define ARM_UART_MMIO_SIZE	0x10000

#define ARM_RTC_MMIO_BASE	(ARM_UART_MMIO_BASE + ARM_UART_MMIO_SIZE)
#define ARM_RTC_MMIO_SIZE	0x10000

#define ARM_PVTIME_BASE		(ARM_RTC_MMIO_BASE + ARM_RTC_MMIO_SIZE)
#define ARM_PVTIME_SIZE		SZ_64K

#define KVM_FLASH_MMIO_BASE	(ARM_MMIO_AREA + 0x1000000)
#define KVM_FLASH_MAX_SIZE	0x1000000

#define KVM_VIRTIO_MMIO_AREA	(KVM_FLASH_MMIO_BASE + KVM_FLASH_MAX_SIZE)
#define ARM_VIRTIO_MMIO_SIZE	(ARM_AXI_AREA - \
				(KVM_VIRTIO_MMIO_AREA + ARM_GIC_SIZE))

#define ARM_GIC_DIST_BASE	(ARM_AXI_AREA - ARM_GIC_DIST_SIZE)
#define ARM_GIC_CPUI_BASE	(ARM_GIC_DIST_BASE - ARM_GIC_CPUI_SIZE)
#define ARM_GIC_SIZE		(ARM_GIC_DIST_SIZE + ARM_GIC_CPUI_SIZE)
#define ARM_GIC_DIST_SIZE	0x10000
#define ARM_GIC_CPUI_SIZE	0x20000


#define KVM_PCI_CFG_AREA	ARM_AXI_AREA
#define ARM_PCI_CFG_SIZE	(1ULL << 28)
#define KVM_PCI_MMIO_AREA	(KVM_PCI_CFG_AREA + ARM_PCI_CFG_SIZE)
#define ARM_PCI_MMIO_SIZE	(ARM_MEMORY_AREA - \
				(ARM_AXI_AREA + ARM_PCI_CFG_SIZE))


#define ARM_LOMAP_MAX_MEMORY	((1ULL << 32) - ARM_MEMORY_AREA)


#define KVM_IOEVENTFD_HAS_PIO	0

/*
 * On a GICv3 there must be one redistributor per vCPU.
 * The value here is the size for one, we multiply this at runtime with
 * the number of requested vCPUs to get the actual size.
 */
#define ARM_GIC_REDIST_SIZE	0x20000

#define KVM_IRQ_OFFSET		GIC_SPI_IRQ_BASE

#define KVM_VM_TYPE		0

#define VIRTIO_RING_ENDIAN	(VIRTIO_ENDIAN_LE | VIRTIO_ENDIAN_BE)

#define ARCH_HAS_PCI_EXP	1

static inline bool arm_addr_in_ioport_region(u64 phys_addr)
{
	u64 limit = KVM_IOPORT_AREA + ARM_IOPORT_SIZE;
	return phys_addr >= KVM_IOPORT_AREA && phys_addr < limit;
}

struct kvm_arch {
	/*
	 * We may have to align the guest memory for virtio, so keep the
	 * original pointers here for munmap.
	 */
	void	*ram_alloc_start;
	u64	ram_alloc_size;

	/*
	 * Guest addresses for memory layout.
	 */
	u64	memory_guest_start;
	u64	kern_guest_start;
	u64	initrd_guest_start;
	u64	initrd_size;
	u64	dtb_guest_start;

	cpu_set_t *vcpu_affinity_cpuset;
};

#endif /* ARM_COMMON__KVM_ARCH_H */
