#include "kvm/vesa.h"

#include "kvm/devices.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/framebuffer.h"
#include "kvm/kvm-cpu.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"

#include <linux/byteorder.h>
#include <sys/mman.h>
#include <linux/err.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <unistd.h>

static struct pci_device_header vesa_pci_device = {
	.vendor_id	= cpu_to_le16(PCI_VENDOR_ID_REDHAT_QUMRANET),
	.device_id	= cpu_to_le16(PCI_DEVICE_ID_VESA),
	.header_type	= PCI_HEADER_TYPE_NORMAL,
	.revision_id	= 0,
	.class[2]	= 0x03,
	.subsys_vendor_id = cpu_to_le16(PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET),
	.subsys_id	= cpu_to_le16(PCI_SUBSYSTEM_ID_VESA),
	.bar[1]		= cpu_to_le32(VESA_MEM_ADDR | PCI_BASE_ADDRESS_SPACE_MEMORY),
	.bar_size[1]	= VESA_MEM_SIZE,
};

static struct device_header vesa_device = {
	.bus_type	= DEVICE_BUS_PCI,
	.data		= &vesa_pci_device,
};

static struct framebuffer vesafb = {
	.width		= VESA_WIDTH,
	.height		= VESA_HEIGHT,
	.depth		= VESA_BPP,
	.mem_addr	= VESA_MEM_ADDR,
	.mem_size	= VESA_MEM_SIZE,
};

static void vesa_pci_io(struct kvm_cpu *vcpu, u64 addr, u8 *data, u32 len,
		        u8 is_write, void *ptr)
{
}

static int vesa__bar_activate(struct kvm *kvm, struct pci_device_header *pci_hdr,
			      int bar_num, void *data)
{
	/* We don't support remapping of the framebuffer. */
	return 0;
}

static int vesa__bar_deactivate(struct kvm *kvm, struct pci_device_header *pci_hdr,
				int bar_num, void *data)
{
	/* We don't support remapping of the framebuffer. */
	return -EINVAL;
}

struct framebuffer *vesa__init(struct kvm *kvm)
{
	u16 vesa_base_addr;
	char *mem;
	int r;

	BUILD_BUG_ON(!is_power_of_two(VESA_MEM_SIZE));
	BUILD_BUG_ON(VESA_MEM_SIZE < VESA_BPP/8 * VESA_WIDTH * VESA_HEIGHT);

	vesa_base_addr = pci_get_io_port_block(PCI_IO_SIZE);
	r = kvm__register_pio(kvm, vesa_base_addr, PCI_IO_SIZE, vesa_pci_io,
			      NULL);
	if (r < 0)
		goto out_error;

	vesa_pci_device.bar[0]		= cpu_to_le32(vesa_base_addr | PCI_BASE_ADDRESS_SPACE_IO);
	vesa_pci_device.bar_size[0]	= PCI_IO_SIZE;
	r = pci__register_bar_regions(kvm, &vesa_pci_device, vesa__bar_activate,
				      vesa__bar_deactivate, NULL);
	if (r < 0)
		goto unregister_ioport;

	r = device__register(&vesa_device);
	if (r < 0)
		goto unregister_ioport;

	mem = mmap(NULL, VESA_MEM_SIZE, PROT_RW, MAP_ANON_NORESERVE, -1, 0);
	if (mem == MAP_FAILED) {
		r = -errno;
		goto unregister_device;
	}

	r = kvm__register_dev_mem(kvm, VESA_MEM_ADDR, VESA_MEM_SIZE, mem);
	if (r < 0)
		goto unmap_dev;

	vesafb.mem = mem;
	vesafb.kvm = kvm;
	return fb__register(&vesafb);

unmap_dev:
	munmap(mem, VESA_MEM_SIZE);
unregister_device:
	device__unregister(&vesa_device);
unregister_ioport:
	kvm__deregister_pio(kvm, vesa_base_addr);
out_error:
	return ERR_PTR(r);
}
