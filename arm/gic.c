#include "kvm/fdt.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"

#include "arm-common/gic.h"

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/sizes.h>

#define IRQCHIP_GIC 0

static int gic_fd = -1;
static u64 gic_redists_base;
static u64 gic_redists_size;
static u64 gic_msi_base;
static u64 gic_msi_size = 0;
static bool vgic_is_init = false;

struct kvm_irqfd_line {
	unsigned int		gsi;
	int			trigger_fd;
	int			resample_fd;
	struct list_head	list;
};

static LIST_HEAD(irqfd_lines);

int irqchip_parser(const struct option *opt, const char *arg, int unset)
{
	enum irqchip_type *type = opt->value;

	if (!strcmp(arg, "gicv2")) {
		*type = IRQCHIP_GICV2;
	} else if (!strcmp(arg, "gicv2m")) {
		*type = IRQCHIP_GICV2M;
	} else if (!strcmp(arg, "gicv3")) {
		*type = IRQCHIP_GICV3;
	} else if (!strcmp(arg, "gicv3-its")) {
		*type = IRQCHIP_GICV3_ITS;
	} else {
		pr_err("irqchip: unknown type \"%s\"\n", arg);
		return -1;
	}

	return 0;
}

static int irq__setup_irqfd_lines(struct kvm *kvm)
{
	int ret;
	struct kvm_irqfd_line *line, *tmp;

	list_for_each_entry_safe(line, tmp, &irqfd_lines, list) {
		ret = irq__common_add_irqfd(kvm, line->gsi, line->trigger_fd,
					    line->resample_fd);
		if (ret < 0) {
			pr_err("Failed to register IRQFD");
			return ret;
		}

		list_del(&line->list);
		free(line);
	}

	return 0;
}

static int irq__routing_init(struct kvm *kvm)
{
	int r;
	int irqlines = ALIGN(irq__get_nr_allocated_lines(), 32);

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
				.u.irqchip.irqchip = IRQCHIP_GIC,
				.u.irqchip.pin = next_gsi,
		};
	}

	return 0;
}

static int gic__create_its_frame(struct kvm *kvm, u64 its_frame_addr)
{
	struct kvm_create_device its_device = {
		.type = KVM_DEV_TYPE_ARM_VGIC_ITS,
		.flags	= 0,
	};
	struct kvm_device_attr its_attr = {
		.group	= KVM_DEV_ARM_VGIC_GRP_ADDR,
		.attr	= KVM_VGIC_ITS_ADDR_TYPE,
		.addr	= (u64)(unsigned long)&its_frame_addr,
	};
	struct kvm_device_attr its_init_attr = {
		.group	= KVM_DEV_ARM_VGIC_GRP_CTRL,
		.attr	= KVM_DEV_ARM_VGIC_CTRL_INIT,
	};
	int err;

	err = ioctl(kvm->vm_fd, KVM_CREATE_DEVICE, &its_device);
	if (err) {
		fprintf(stderr,
			"GICv3 ITS requested, but kernel does not support it.\n");
		fprintf(stderr, "Try --irqchip=gicv3 instead\n");
		return err;
	}

	err = ioctl(its_device.fd, KVM_HAS_DEVICE_ATTR, &its_attr);
	if (err) {
		close(its_device.fd);
		its_device.fd = -1;
		return err;
	}

	err = ioctl(its_device.fd, KVM_SET_DEVICE_ATTR, &its_attr);
	if (err)
		return err;

	return ioctl(its_device.fd, KVM_SET_DEVICE_ATTR, &its_init_attr);
}

static int gic__create_msi_frame(struct kvm *kvm, enum irqchip_type type,
				 u64 msi_frame_addr)
{
	switch (type) {
	case IRQCHIP_GICV2M:
		return gic__create_gicv2m_frame(kvm, msi_frame_addr);
	case IRQCHIP_GICV3_ITS:
		return gic__create_its_frame(kvm, msi_frame_addr);
	default:	/* No MSI frame needed */
		return 0;
	}
}

static int gic__create_device(struct kvm *kvm, enum irqchip_type type)
{
	int err;
	u64 cpu_if_addr = ARM_GIC_CPUI_BASE;
	u64 dist_addr = ARM_GIC_DIST_BASE;
	struct kvm_create_device gic_device = {
		.flags	= 0,
	};
	struct kvm_device_attr cpu_if_attr = {
		.group	= KVM_DEV_ARM_VGIC_GRP_ADDR,
		.attr	= KVM_VGIC_V2_ADDR_TYPE_CPU,
		.addr	= (u64)(unsigned long)&cpu_if_addr,
	};
	struct kvm_device_attr dist_attr = {
		.group	= KVM_DEV_ARM_VGIC_GRP_ADDR,
		.addr	= (u64)(unsigned long)&dist_addr,
	};
	struct kvm_device_attr redist_attr = {
		.group	= KVM_DEV_ARM_VGIC_GRP_ADDR,
		.attr	= KVM_VGIC_V3_ADDR_TYPE_REDIST,
		.addr	= (u64)(unsigned long)&gic_redists_base,
	};

	switch (type) {
	case IRQCHIP_GICV2M:
	case IRQCHIP_GICV2:
		gic_device.type = KVM_DEV_TYPE_ARM_VGIC_V2;
		dist_attr.attr  = KVM_VGIC_V2_ADDR_TYPE_DIST;
		break;
	case IRQCHIP_GICV3:
	case IRQCHIP_GICV3_ITS:
		gic_device.type = KVM_DEV_TYPE_ARM_VGIC_V3;
		dist_attr.attr  = KVM_VGIC_V3_ADDR_TYPE_DIST;
		break;
	case IRQCHIP_AUTO:
		return -ENODEV;
	}

	err = ioctl(kvm->vm_fd, KVM_CREATE_DEVICE, &gic_device);
	if (err)
		return err;

	gic_fd = gic_device.fd;

	switch (type) {
	case IRQCHIP_GICV2M:
	case IRQCHIP_GICV2:
		err = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &cpu_if_attr);
		break;
	case IRQCHIP_GICV3_ITS:
	case IRQCHIP_GICV3:
		err = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &redist_attr);
		break;
	case IRQCHIP_AUTO:
		return -ENODEV;
	}
	if (err)
		goto out_err;

	err = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &dist_attr);
	if (err)
		goto out_err;

	err = gic__create_msi_frame(kvm, type, gic_msi_base);
	if (err)
		goto out_err;

	return 0;

out_err:
	close(gic_fd);
	gic_fd = -1;
	return err;
}

static int gic__create_irqchip(struct kvm *kvm)
{
	int err;
	struct kvm_arm_device_addr gic_addr[] = {
		[0] = {
			.id = KVM_VGIC_V2_ADDR_TYPE_DIST |
			(KVM_ARM_DEVICE_VGIC_V2 << KVM_ARM_DEVICE_ID_SHIFT),
			.addr = ARM_GIC_DIST_BASE,
		},
		[1] = {
			.id = KVM_VGIC_V2_ADDR_TYPE_CPU |
			(KVM_ARM_DEVICE_VGIC_V2 << KVM_ARM_DEVICE_ID_SHIFT),
			.addr = ARM_GIC_CPUI_BASE,
		}
	};

	err = ioctl(kvm->vm_fd, KVM_CREATE_IRQCHIP);
	if (err)
		return err;

	err = ioctl(kvm->vm_fd, KVM_ARM_SET_DEVICE_ADDR, &gic_addr[0]);
	if (err)
		return err;

	err = ioctl(kvm->vm_fd, KVM_ARM_SET_DEVICE_ADDR, &gic_addr[1]);
	return err;
}

int gic__create(struct kvm *kvm, enum irqchip_type type)
{
	enum irqchip_type try;
	int err;

	switch (type) {
	case IRQCHIP_AUTO:
		for (try = IRQCHIP_GICV3_ITS; try >= IRQCHIP_GICV2; try--) {
			err = gic__create(kvm, try);
			if (!err)
				break;
		}
		if (err)
			return err;

		kvm->cfg.arch.irqchip = try;
		return 0;
	case IRQCHIP_GICV2M:
		gic_msi_size = KVM_VGIC_V2M_SIZE;
		gic_msi_base = ARM_GIC_CPUI_BASE - gic_msi_size;
		break;
	case IRQCHIP_GICV2:
		break;
	case IRQCHIP_GICV3_ITS:
		/* The 64K page with the doorbell is included. */
		gic_msi_size = KVM_VGIC_V3_ITS_SIZE;
		/* fall through */
	case IRQCHIP_GICV3:
		gic_redists_size = kvm->cfg.nrcpus * ARM_GIC_REDIST_SIZE;
		gic_redists_base = ARM_GIC_DIST_BASE - gic_redists_size;
		gic_msi_base = gic_redists_base - gic_msi_size;
		break;
	default:
		return -ENODEV;
	}

	/* Try the new way first, and fallback on legacy method otherwise */
	err = gic__create_device(kvm, type);
	if (err && type == IRQCHIP_GICV2)
		err = gic__create_irqchip(kvm);

	return err;
}

/*
 * Sets the number of used interrupts and finalizes the GIC init explicitly.
 */
static int gic__init_gic(struct kvm *kvm)
{
	int ret;

	int lines = irq__get_nr_allocated_lines();
	u32 nr_irqs = ALIGN(lines, 32) + GIC_SPI_IRQ_BASE;
	struct kvm_device_attr nr_irqs_attr = {
		.group	= KVM_DEV_ARM_VGIC_GRP_NR_IRQS,
		.addr	= (u64)(unsigned long)&nr_irqs,
	};
	struct kvm_device_attr vgic_init_attr = {
		.group	= KVM_DEV_ARM_VGIC_GRP_CTRL,
		.attr	= KVM_DEV_ARM_VGIC_CTRL_INIT,
	};

	/*
	 * If we didn't use the KVM_CREATE_DEVICE method, KVM will
	 * give us some default number of interrupts. The GIC initialization
	 * will be done automatically in this case.
	 */
	if (gic_fd < 0)
		return 0;

	if (!ioctl(gic_fd, KVM_HAS_DEVICE_ATTR, &nr_irqs_attr)) {
		ret = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &nr_irqs_attr);
		if (ret)
			return ret;
	}

	irq__routing_init(kvm);

	if (!ioctl(gic_fd, KVM_HAS_DEVICE_ATTR, &vgic_init_attr)) {
		ret = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &vgic_init_attr);
		if (ret)
			return ret;
	}

	kvm->msix_needs_devid = kvm__supports_vm_extension(kvm,
							   KVM_CAP_MSI_DEVID);

	vgic_is_init = true;

	return irq__setup_irqfd_lines(kvm);
}
late_init(gic__init_gic)

void gic__generate_fdt_nodes(void *fdt, enum irqchip_type type)
{
	const char *compatible, *msi_compatible = NULL;
	u64 msi_prop[2];
	u64 reg_prop[] = {
		cpu_to_fdt64(ARM_GIC_DIST_BASE), cpu_to_fdt64(ARM_GIC_DIST_SIZE),
		0, 0,				/* to be filled */
	};

	switch (type) {
	case IRQCHIP_GICV2M:
		msi_compatible = "arm,gic-v2m-frame";
		/* fall-through */
	case IRQCHIP_GICV2:
		compatible = "arm,cortex-a15-gic";
		reg_prop[2] = cpu_to_fdt64(ARM_GIC_CPUI_BASE);
		reg_prop[3] = cpu_to_fdt64(ARM_GIC_CPUI_SIZE);
		break;
	case IRQCHIP_GICV3_ITS:
		msi_compatible = "arm,gic-v3-its";
		/* fall-through */
	case IRQCHIP_GICV3:
		compatible = "arm,gic-v3";
		reg_prop[2] = cpu_to_fdt64(gic_redists_base);
		reg_prop[3] = cpu_to_fdt64(gic_redists_size);
		break;
	default:
		return;
	}

	_FDT(fdt_begin_node(fdt, "intc"));
	_FDT(fdt_property_string(fdt, "compatible", compatible));
	_FDT(fdt_property_cell(fdt, "#interrupt-cells", GIC_FDT_IRQ_NUM_CELLS));
	_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_GIC));
	_FDT(fdt_property_cell(fdt, "#address-cells", 2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 2));

	if (msi_compatible) {
		_FDT(fdt_property(fdt, "ranges", NULL, 0));

		_FDT(fdt_begin_node(fdt, "msic"));
		_FDT(fdt_property_string(fdt, "compatible", msi_compatible));
		_FDT(fdt_property(fdt, "msi-controller", NULL, 0));
		_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_MSI));
		msi_prop[0] = cpu_to_fdt64(gic_msi_base);
		msi_prop[1] = cpu_to_fdt64(gic_msi_size);
		_FDT(fdt_property(fdt, "reg", msi_prop, sizeof(msi_prop)));
		_FDT(fdt_end_node(fdt));
	}

	_FDT(fdt_end_node(fdt));
}

u32 gic__get_fdt_irq_cpumask(struct kvm *kvm)
{
	/* Only for GICv2 */
	if (kvm->cfg.arch.irqchip == IRQCHIP_GICV3 ||
	    kvm->cfg.arch.irqchip == IRQCHIP_GICV3_ITS)
		return 0;

	if (kvm->nrcpus > 8)
		return GIC_FDT_IRQ_PPI_CPU_MASK;

	return ((1U << kvm->nrcpus) - 1) << GIC_FDT_IRQ_PPI_CPU_SHIFT;
}

#define KVM_IRQCHIP_IRQ(x) (KVM_ARM_IRQ_TYPE_SPI << KVM_ARM_IRQ_TYPE_SHIFT) |\
			   ((x) & KVM_ARM_IRQ_NUM_MASK)

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	struct kvm_irq_level irq_level = {
		.irq	= KVM_IRQCHIP_IRQ(irq),
		.level	= !!level,
	};

	if (irq < GIC_SPI_IRQ_BASE || irq > GIC_MAX_IRQ)
		pr_warning("Ignoring invalid GIC IRQ %d", irq);
	else if (ioctl(kvm->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
		pr_warning("Could not KVM_IRQ_LINE for irq %d", irq);
}

void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	kvm__irq_line(kvm, irq, VIRTIO_IRQ_HIGH);
	kvm__irq_line(kvm, irq, VIRTIO_IRQ_LOW);
}

int gic__add_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd,
		   int resample_fd)
{
	struct kvm_irqfd_line *line;

	if (vgic_is_init)
		return irq__common_add_irqfd(kvm, gsi, trigger_fd, resample_fd);

	/* Postpone the routing setup until we have a distributor */
	line = malloc(sizeof(*line));
	if (!line)
		return -ENOMEM;

	*line = (struct kvm_irqfd_line) {
		.gsi		= gsi,
		.trigger_fd	= trigger_fd,
		.resample_fd	= resample_fd,
	};
	list_add(&line->list, &irqfd_lines);

	return 0;
}

void gic__del_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd)
{
	struct kvm_irqfd_line *line;

	if (vgic_is_init) {
		irq__common_del_irqfd(kvm, gsi, trigger_fd);
		return;
	}

	list_for_each_entry(line, &irqfd_lines, list) {
		if (line->gsi != gsi)
			continue;

		list_del(&line->list);
		free(line);
		break;
	}
}
