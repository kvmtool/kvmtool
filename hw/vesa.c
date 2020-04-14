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

static bool vesa_pci_io_in(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	return true;
}

static bool vesa_pci_io_out(struct ioport *ioport, struct kvm_cpu *vcpu, u16 port, void *data, int size)
{
	return true;
}

static struct ioport_operations vesa_io_ops = {
	.io_in			= vesa_pci_io_in,
	.io_out			= vesa_pci_io_out,
};

static struct pci_device_header vesa_pci_device = {
	.vendor_id		= cpu_to_le16(PCI_VENDOR_ID_REDHAT_QUMRANET),
	.device_id		= cpu_to_le16(PCI_DEVICE_ID_VESA),
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class[2]		= 0x03,
	.subsys_vendor_id	= cpu_to_le16(PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET),
	.subsys_id		= cpu_to_le16(PCI_SUBSYSTEM_ID_VESA),
	.bar[1]			= cpu_to_le32(VESA_MEM_ADDR | PCI_BASE_ADDRESS_SPACE_MEMORY),
	.bar_size[1]		= VESA_MEM_SIZE,
};

static struct device_header vesa_device = {
	.bus_type	= DEVICE_BUS_PCI,
	.data		= &vesa_pci_device,
};

static struct framebuffer vesafb;

struct framebuffer *vesa__init(struct kvm *kvm)
{
	u16 vesa_base_addr;
	char *mem;
	int r;

	BUILD_BUG_ON(!is_power_of_two(VESA_MEM_SIZE));
	BUILD_BUG_ON(VESA_MEM_SIZE < VESA_BPP/8 * VESA_WIDTH * VESA_HEIGHT);

	if (!kvm->cfg.vnc && !kvm->cfg.sdl && !kvm->cfg.gtk)
		return NULL;
	vesa_base_addr = pci_get_io_port_block(PCI_IO_SIZE);
	r = ioport__register(kvm, vesa_base_addr, &vesa_io_ops, PCI_IO_SIZE, NULL);
	if (r < 0)
		goto out_error;

	vesa_pci_device.bar[0]		= cpu_to_le32(vesa_base_addr | PCI_BASE_ADDRESS_SPACE_IO);
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

	vesafb = (struct framebuffer) {
		.width			= VESA_WIDTH,
		.height			= VESA_HEIGHT,
		.depth			= VESA_BPP,
		.mem			= mem,
		.mem_addr		= VESA_MEM_ADDR,
		.mem_size		= VESA_MEM_SIZE,
		.kvm			= kvm,
	};
	return fb__register(&vesafb);

unmap_dev:
	munmap(mem, VESA_MEM_SIZE);
unregister_device:
	device__unregister(&vesa_device);
unregister_ioport:
	ioport__unregister(kvm, vesa_base_addr);
out_error:
	return ERR_PTR(r);
}
