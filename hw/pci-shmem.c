#include "kvm/pci-shmem.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/util.h"
#include "kvm/ioport.h"
#include "kvm/ioeventfd.h"

#include <linux/kvm.h>
#include <linux/byteorder.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/mman.h>

static struct pci_device_header pci_shmem_pci_device = {
	.vendor_id	= cpu_to_le16(PCI_VENDOR_ID_REDHAT_QUMRANET),
	.device_id	= cpu_to_le16(0x1110),
	.header_type	= PCI_HEADER_TYPE_NORMAL,
	.class[2]	= 0xFF,	/* misc pci device */
	.status		= cpu_to_le16(PCI_STATUS_CAP_LIST),
	.capabilities	= (void *)&pci_shmem_pci_device.msix - (void *)&pci_shmem_pci_device,
	.msix.cap	= PCI_CAP_ID_MSIX,
	.msix.ctrl	= cpu_to_le16(1),
	.msix.table_offset = cpu_to_le32(1),		/* Use BAR 1 */
	.msix.pba_offset = cpu_to_le32(0x1001),		/* Use BAR 1 */
};

/* registers for the Inter-VM shared memory device */
enum ivshmem_registers {
	INTRMASK = 0,
	INTRSTATUS = 4,
	IVPOSITION = 8,
	DOORBELL = 12,
};

static struct shmem_info *shmem_region;
static u16 ivshmem_registers;
static int local_fd;
static u32 local_id;
static u64 msix_block;
static u64 msix_pba;
static struct msix_table msix_table[2];

int pci_shmem__register_mem(struct shmem_info *si)
{
	if (shmem_region == NULL) {
		shmem_region = si;
	} else {
		pr_warning("only single shmem currently avail. ignoring.\n");
		free(si);
	}
	return 0;
}

static bool shmem_pci__io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	u16 offset = port - ivshmem_registers;

	switch (offset) {
	case INTRMASK:
		break;
	case INTRSTATUS:
		break;
	case IVPOSITION:
		ioport__write32(data, local_id);
		break;
	case DOORBELL:
		break;
	};

	return true;
}

static bool shmem_pci__io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	u16 offset = port - ivshmem_registers;

	switch (offset) {
	case INTRMASK:
		break;
	case INTRSTATUS:
		break;
	case IVPOSITION:
		break;
	case DOORBELL:
		break;
	};

	return true;
}

static struct ioport_operations shmem_pci__io_ops = {
	.io_in	= shmem_pci__io_in,
	.io_out	= shmem_pci__io_out,
};

static void callback_mmio_msix(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr)
{
	void *mem;

	if (addr - msix_block < 0x1000)
		mem = &msix_table;
	else
		mem = &msix_pba;

	if (is_write)
		memcpy(mem + addr - msix_block, data, len);
	else
		memcpy(data, mem + addr - msix_block, len);
}

/*
 * Return an irqfd which can be used by other guests to signal this guest
 * whenever they need to poke it
 */
int pci_shmem__get_local_irqfd(struct kvm *kvm)
{
	int fd, gsi, r;
	struct kvm_irqfd irqfd;

	if (local_fd == 0) {
		fd = eventfd(0, 0);
		if (fd < 0)
			return fd;

		if (pci_shmem_pci_device.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_ENABLE)) {
			gsi = irq__add_msix_route(kvm, &msix_table[0].msg);
		} else {
			gsi = pci_shmem_pci_device.irq_line;
		}

		irqfd = (struct kvm_irqfd) {
			.fd = fd,
			.gsi = gsi,
		};

		r = ioctl(kvm->vm_fd, KVM_IRQFD, &irqfd);
		if (r < 0)
			return r;

		local_fd = fd;
	}

	return local_fd;
}

/*
 * Connect a new client to ivshmem by adding the appropriate datamatch
 * to the DOORBELL
 */
int pci_shmem__add_client(struct kvm *kvm, u32 id, int fd)
{
	struct kvm_ioeventfd ioevent;

	ioevent = (struct kvm_ioeventfd) {
		.addr		= ivshmem_registers + DOORBELL,
		.len		= sizeof(u32),
		.datamatch	= id,
		.fd		= fd,
		.flags		= KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DATAMATCH,
	};

	return ioctl(kvm->vm_fd, KVM_IOEVENTFD, &ioevent);
}

/*
 * Remove a client connected to ivshmem by removing the appropriate datamatch
 * from the DOORBELL
 */
int pci_shmem__remove_client(struct kvm *kvm, u32 id)
{
	struct kvm_ioeventfd ioevent;

	ioevent = (struct kvm_ioeventfd) {
		.addr		= ivshmem_registers + DOORBELL,
		.len		= sizeof(u32),
		.datamatch	= id,
		.flags		= KVM_IOEVENTFD_FLAG_PIO
				| KVM_IOEVENTFD_FLAG_DATAMATCH
				| KVM_IOEVENTFD_FLAG_DEASSIGN,
	};

	return ioctl(kvm->vm_fd, KVM_IOEVENTFD, &ioevent);
}

static void *setup_shmem(const char *key, size_t len, int creating)
{
	int fd;
	int rtn;
	void *mem;
	int flag = O_RDWR;

	if (creating)
		flag |= O_CREAT;

	fd = shm_open(key, flag, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		pr_warning("Failed to open shared memory file %s\n", key);
		return NULL;
	}

	if (creating) {
		rtn = ftruncate(fd, (off_t) len);
		if (rtn < 0)
			pr_warning("Can't ftruncate(fd,%zu)\n", len);
	}
	mem = mmap(NULL, len,
		   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0);
	if (mem == MAP_FAILED) {
		pr_warning("Failed to mmap shared memory file");
		mem = NULL;
	}
	close(fd);

	return mem;
}

int pci_shmem__init(struct kvm *kvm)
{
	u8 dev, line, pin;
	char *mem;
	int r;

	if (shmem_region == 0)
		return 0;

	/* Register good old INTx */
	if (irq__register_device(PCI_DEVICE_ID_PCI_SHMEM, &dev, &pin, &line) < 0)
		return 0;

	pci_shmem_pci_device.irq_pin = pin;
	pci_shmem_pci_device.irq_line = line;

	/* Register MMIO space for MSI-X */
	r = ioport__register(IOPORT_EMPTY, &shmem_pci__io_ops, IOPORT_SIZE, NULL);
	if (r < 0)
		return r;
	ivshmem_registers = (u16)r;

	msix_block = pci_get_io_space_block(0x1010);
	kvm__register_mmio(kvm, msix_block, 0x1010, false, callback_mmio_msix, NULL);

	/*
	 * This registers 3 BARs:
	 *
	 * 0 - ivshmem registers
	 * 1 - MSI-X MMIO space
	 * 2 - Shared memory block
	 */
	pci_shmem_pci_device.bar[0] = cpu_to_le32(ivshmem_registers | PCI_BASE_ADDRESS_SPACE_IO);
	pci_shmem_pci_device.bar_size[0] = shmem_region->size;
	pci_shmem_pci_device.bar[1] = cpu_to_le32(msix_block | PCI_BASE_ADDRESS_SPACE_MEMORY);
	pci_shmem_pci_device.bar_size[1] = 0x1010;
	pci_shmem_pci_device.bar[2] = cpu_to_le32(shmem_region->phys_addr | PCI_BASE_ADDRESS_SPACE_MEMORY);
	pci_shmem_pci_device.bar_size[2] = shmem_region->size;

	pci__register(&pci_shmem_pci_device, dev);

	/* Open shared memory and plug it into the guest */
	mem = setup_shmem(shmem_region->handle, shmem_region->size,
				shmem_region->create);
	if (mem == NULL)
		return 0;
	kvm__register_mem(kvm, shmem_region->phys_addr, shmem_region->size,
			  mem);
	return 1;
}
