#include "kvm/vesa.h"

#include "kvm/virtio-pci-dev.h"
#include "kvm/framebuffer.h"
#include "kvm/kvm-cpu.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include <sys/mman.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <unistd.h>

static bool vesa_pci_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	return true;
}

static bool vesa_pci_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	return true;
}

static struct ioport_operations vesa_io_ops = {
	.io_in			= vesa_pci_io_in,
	.io_out			= vesa_pci_io_out,
};

static struct pci_device_header vesa_pci_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VESA,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x030000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= PCI_SUBSYSTEM_ID_VESA,
	.bar[1]			= VESA_MEM_ADDR | PCI_BASE_ADDRESS_SPACE_MEMORY,
	.bar_size[1]		= VESA_MEM_SIZE,
};

static struct framebuffer vesafb;

struct framebuffer *vesa__init(struct kvm *kvm)
{
	u16 vesa_base_addr;
	u8 dev, line, pin;
	char *mem;

	if (irq__register_device(PCI_DEVICE_ID_VESA, &dev, &pin, &line) < 0)
		return NULL;

	vesa_pci_device.irq_pin		= pin;
	vesa_pci_device.irq_line	= line;
	vesa_base_addr			= ioport__register(IOPORT_EMPTY, &vesa_io_ops, IOPORT_SIZE, NULL);
	vesa_pci_device.bar[0]		= vesa_base_addr | PCI_BASE_ADDRESS_SPACE_IO;
	pci__register(&vesa_pci_device, dev);

	mem = mmap(NULL, VESA_MEM_SIZE, PROT_RW, MAP_ANON_NORESERVE, -1, 0);
	if (mem == MAP_FAILED)
		return NULL;

	kvm__register_mem(kvm, VESA_MEM_ADDR, VESA_MEM_SIZE, mem);

	vesafb = (struct framebuffer) {
		.width			= VESA_WIDTH,
		.height			= VESA_HEIGHT,
		.depth			= VESA_BPP,
		.mem			= mem,
		.mem_addr		= VESA_MEM_ADDR,
		.mem_size		= VESA_MEM_SIZE,
	};
	return fb__register(&vesafb);
}
