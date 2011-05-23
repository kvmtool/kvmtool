#include "kvm/vesa.h"

#include "kvm/virtio-pci-dev.h"
#include "kvm/kvm-cpu.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <unistd.h>

#include <rfb/rfb.h>

#define VESA_QUEUE_SIZE		128
#define VESA_IRQ		14

/*
 * This "6000" value is pretty much the result of experimentation
 * It seems that around this value, things update pretty smoothly
 */
#define VESA_UPDATE_TIME	6000

static char videomem[VESA_MEM_SIZE];

static bool vesa_pci_io_in(struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	return true;
}

static bool vesa_pci_io_out(struct kvm *kvm, u16 port, void *data, int size, u32 count)
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
	.bar[0]			= IOPORT_VESA   | PCI_BASE_ADDRESS_SPACE_IO,
	.bar[1]			= VESA_MEM_ADDR | PCI_BASE_ADDRESS_SPACE_MEMORY,
};


void vesa_mmio_callback(u64 addr, u8 *data, u32 len, u8 is_write)
{
	if (!is_write)
		return;

	memcpy(&videomem[addr - VESA_MEM_ADDR], data, len);
}

void vesa__init(struct kvm *kvm)
{
	u8 dev, line, pin;
	pthread_t thread;

	if (irq__register_device(PCI_DEVICE_ID_VESA, &dev, &pin, &line) < 0)
		return;

	vesa_pci_device.irq_pin		= pin;
	vesa_pci_device.irq_line	= line;

	pci__register(&vesa_pci_device, dev);

	ioport__register(IOPORT_VESA, &vesa_io_ops, IOPORT_VESA_SIZE);

	kvm__register_mmio(VESA_MEM_ADDR, VESA_MEM_SIZE, &vesa_mmio_callback);

	pthread_create(&thread, NULL, vesa__dovnc, kvm);
}

/*
 * This starts a VNC server to display the framebuffer.
 * It's not altogether clear this belongs here rather than in kvm-run.c
 */
void *vesa__dovnc(void *v)
{
	/*
	 * Make a fake argc and argv because the getscreen function
	 * seems to want it.
	 */
	char argv[1][1] = {{0}};
	int argc = 1;

	rfbScreenInfoPtr server;

	server = rfbGetScreen(&argc, (char **) argv, VESA_WIDTH, VESA_HEIGHT, 8, 3, 4);
	server->frameBuffer		= videomem;
	server->alwaysShared		= TRUE;
	rfbInitServer(server);

	while (rfbIsActive(server)) {
		rfbMarkRectAsModified(server, 0, 0, VESA_WIDTH, VESA_HEIGHT);
		rfbProcessEvents(server, server->deferUpdateTime * VESA_UPDATE_TIME);
	}
	return NULL;
}
