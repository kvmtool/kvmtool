#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/ioeventfd.h"
#include "kvm/ioport.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/irq.h"
#include "kvm/util.h"

static int aia_fd = -1;

static u32 aia_mode = KVM_DEV_RISCV_AIA_MODE_EMUL;
static struct kvm_device_attr aia_mode_attr = {
	.group	= KVM_DEV_RISCV_AIA_GRP_CONFIG,
	.attr	= KVM_DEV_RISCV_AIA_CONFIG_MODE,
};

static u32 aia_nr_ids = 0;
static struct kvm_device_attr aia_nr_ids_attr = {
	.group	= KVM_DEV_RISCV_AIA_GRP_CONFIG,
	.attr	= KVM_DEV_RISCV_AIA_CONFIG_IDS,
};

static u32 aia_nr_sources = 0;
static struct kvm_device_attr aia_nr_sources_attr = {
	.group	= KVM_DEV_RISCV_AIA_GRP_CONFIG,
	.attr	= KVM_DEV_RISCV_AIA_CONFIG_SRCS,
};

static u32 aia_hart_bits = 0;
static struct kvm_device_attr aia_hart_bits_attr = {
	.group	= KVM_DEV_RISCV_AIA_GRP_CONFIG,
	.attr	= KVM_DEV_RISCV_AIA_CONFIG_HART_BITS,
};

static u32 aia_nr_harts = 0;

#define IRQCHIP_AIA_NR			0

#define AIA_IMSIC_BASE			RISCV_IRQCHIP
#define AIA_IMSIC_ADDR(__hart)		\
	(AIA_IMSIC_BASE + (__hart) * KVM_DEV_RISCV_IMSIC_SIZE)
#define AIA_IMSIC_SIZE			\
	(aia_nr_harts * KVM_DEV_RISCV_IMSIC_SIZE)
#define AIA_APLIC_ADDR			\
	(AIA_IMSIC_BASE + AIA_IMSIC_SIZE)

static void aia__generate_fdt_node(void *fdt, struct kvm *kvm)
{
	u32 i;
	char name[64];
	u32 reg_cells[4], *irq_cells;

	irq_cells = calloc(aia_nr_harts * 2, sizeof(u32));
	if (!irq_cells)
		die("Failed to alloc irq_cells");

	sprintf(name, "imsics@%08x", (u32)AIA_IMSIC_BASE);
	_FDT(fdt_begin_node(fdt, name));
	_FDT(fdt_property_string(fdt, "compatible", "riscv,imsics"));
	reg_cells[0] = 0;
	reg_cells[1] = cpu_to_fdt32(AIA_IMSIC_BASE);
	reg_cells[2] = 0;
	reg_cells[3] = cpu_to_fdt32(AIA_IMSIC_SIZE);
	_FDT(fdt_property(fdt, "reg", reg_cells, sizeof(reg_cells)));
	_FDT(fdt_property_cell(fdt, "#interrupt-cells", 0));
	_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
	_FDT(fdt_property(fdt, "msi-controller", NULL, 0));
	_FDT(fdt_property_cell(fdt, "riscv,num-ids", aia_nr_ids));
	_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_AIA_IMSIC));
	for (i = 0; i < aia_nr_harts; i++) {
		irq_cells[2*i + 0] = cpu_to_fdt32(PHANDLE_CPU_INTC_BASE + i);
		irq_cells[2*i + 1] = cpu_to_fdt32(9);
	}
	_FDT(fdt_property(fdt, "interrupts-extended", irq_cells,
			  sizeof(u32) * aia_nr_harts * 2));
	_FDT(fdt_end_node(fdt));

	free(irq_cells);

	/* Skip APLIC node if we have no interrupt sources */
	if (!aia_nr_sources)
		return;

	sprintf(name, "aplic@%08x", (u32)AIA_APLIC_ADDR);
	_FDT(fdt_begin_node(fdt, name));
	_FDT(fdt_property_string(fdt, "compatible", "riscv,aplic"));
	reg_cells[0] = 0;
	reg_cells[1] = cpu_to_fdt32(AIA_APLIC_ADDR);
	reg_cells[2] = 0;
	reg_cells[3] = cpu_to_fdt32(KVM_DEV_RISCV_APLIC_SIZE);
	_FDT(fdt_property(fdt, "reg", reg_cells, sizeof(reg_cells)));
	_FDT(fdt_property_cell(fdt, "#interrupt-cells", 2));
	_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
	_FDT(fdt_property_cell(fdt, "riscv,num-sources", aia_nr_sources));
	_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_AIA_APLIC));
	_FDT(fdt_property_cell(fdt, "msi-parent", PHANDLE_AIA_IMSIC));
	_FDT(fdt_end_node(fdt));
}

static int aia__irq_routing_init(struct kvm *kvm)
{
	int r;
	int irqlines = aia_nr_sources + 1;

	/* Skip this if we have no interrupt sources */
	if (!aia_nr_sources)
		return 0;

	/*
	 * This describes the default routing that the kernel uses without
	 * any routing explicitly set up via KVM_SET_GSI_ROUTING. So we
	 * don't need to commit these setting right now. The first actual
	 * user (MSI routing) will engage these mappings then.
	 */
	for (next_gsi = 0; next_gsi < irqlines; next_gsi++) {
		r = irq__allocate_routing_entry();
		if (r)
			return r;

		irq_routing->entries[irq_routing->nr++] =
			(struct kvm_irq_routing_entry) {
				.gsi = next_gsi,
				.type = KVM_IRQ_ROUTING_IRQCHIP,
				.u.irqchip.irqchip = IRQCHIP_AIA_NR,
				.u.irqchip.pin = next_gsi,
		};
	}

	return 0;
}

static int aia__init(struct kvm *kvm)
{
	int i, ret;
	u64 aia_addr = 0;
	struct kvm_device_attr aia_addr_attr = {
		.group	= KVM_DEV_RISCV_AIA_GRP_ADDR,
		.addr	= (u64)(unsigned long)&aia_addr,
	};
	struct kvm_device_attr aia_init_attr = {
		.group	= KVM_DEV_RISCV_AIA_GRP_CTRL,
		.attr	= KVM_DEV_RISCV_AIA_CTRL_INIT,
	};

	/* Setup global device attribute variables */
	aia_mode_attr.addr = (u64)(unsigned long)&aia_mode;
	aia_nr_ids_attr.addr = (u64)(unsigned long)&aia_nr_ids;
	aia_nr_sources_attr.addr = (u64)(unsigned long)&aia_nr_sources;
	aia_hart_bits_attr.addr = (u64)(unsigned long)&aia_hart_bits;

	/* Do nothing if AIA device not created */
	if (aia_fd < 0)
		return 0;

	/* Set/Get AIA device config parameters */
	ret = ioctl(aia_fd, KVM_GET_DEVICE_ATTR, &aia_mode_attr);
	if (ret)
		return ret;
	ret = ioctl(aia_fd, KVM_GET_DEVICE_ATTR, &aia_nr_ids_attr);
	if (ret)
		return ret;
	aia_nr_sources = irq__get_nr_allocated_lines();
	ret = ioctl(aia_fd, KVM_SET_DEVICE_ATTR, &aia_nr_sources_attr);
	if (ret)
		return ret;
	aia_hart_bits = fls_long(kvm->nrcpus);
	ret = ioctl(aia_fd, KVM_SET_DEVICE_ATTR, &aia_hart_bits_attr);
	if (ret)
		return ret;

	/* Save number of HARTs for FDT generation */
	aia_nr_harts = kvm->nrcpus;

	/* Set AIA device addresses */
	aia_addr = AIA_APLIC_ADDR;
	aia_addr_attr.attr = KVM_DEV_RISCV_AIA_ADDR_APLIC;
	ret = ioctl(aia_fd, KVM_SET_DEVICE_ATTR, &aia_addr_attr);
	if (ret)
		return ret;
	for (i = 0; i < kvm->nrcpus; i++) {
		aia_addr = AIA_IMSIC_ADDR(i);
		aia_addr_attr.attr = KVM_DEV_RISCV_AIA_ADDR_IMSIC(i);
		ret = ioctl(aia_fd, KVM_SET_DEVICE_ATTR, &aia_addr_attr);
		if (ret)
			return ret;
	}

	/* Setup default IRQ routing */
	aia__irq_routing_init(kvm);

	/* Initialize the AIA device */
	ret = ioctl(aia_fd, KVM_SET_DEVICE_ATTR, &aia_init_attr);
	if (ret)
		return ret;

	/* Mark IRQFD as ready */
	riscv_irqchip_irqfd_ready = true;

	return 0;
}
late_init(aia__init);

void aia__create(struct kvm *kvm)
{
	int err;
	struct kvm_create_device aia_device = {
		.type = KVM_DEV_TYPE_RISCV_AIA,
		.flags = 0,
	};

	if (kvm->cfg.arch.ext_disabled[KVM_RISCV_ISA_EXT_SSAIA])
		return;

	err = ioctl(kvm->vm_fd, KVM_CREATE_DEVICE, &aia_device);
	if (err)
		return;
	aia_fd = aia_device.fd;

	riscv_irqchip = IRQCHIP_AIA;
	riscv_irqchip_inkernel = true;
	riscv_irqchip_trigger = NULL;
	riscv_irqchip_generate_fdt_node = aia__generate_fdt_node;
	riscv_irqchip_phandle = PHANDLE_AIA_APLIC;
	riscv_irqchip_msi_phandle = PHANDLE_AIA_IMSIC;
	riscv_irqchip_line_sensing = true;
}
