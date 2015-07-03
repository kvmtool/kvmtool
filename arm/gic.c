#include "kvm/fdt.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"

#include "arm-common/gic.h"

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/kvm.h>

/* Those names are not defined for ARM (yet) */
#ifndef KVM_VGIC_V3_ADDR_TYPE_DIST
#define KVM_VGIC_V3_ADDR_TYPE_DIST 2
#endif

#ifndef KVM_VGIC_V3_ADDR_TYPE_REDIST
#define KVM_VGIC_V3_ADDR_TYPE_REDIST 3
#endif

static int gic_fd = -1;
static u64 gic_redists_base;
static u64 gic_redists_size;

int irqchip_parser(const struct option *opt, const char *arg, int unset)
{
	enum irqchip_type *type = opt->value;

	if (!strcmp(arg, "gicv2")) {
		*type = IRQCHIP_GICV2;
	} else if (!strcmp(arg, "gicv3")) {
		*type = IRQCHIP_GICV3;
	} else {
		pr_err("irqchip: unknown type \"%s\"\n", arg);
		return -1;
	}

	return 0;
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
	case IRQCHIP_GICV2:
		gic_device.type = KVM_DEV_TYPE_ARM_VGIC_V2;
		dist_attr.attr  = KVM_VGIC_V2_ADDR_TYPE_DIST;
		break;
	case IRQCHIP_GICV3:
		gic_device.type = KVM_DEV_TYPE_ARM_VGIC_V3;
		dist_attr.attr  = KVM_VGIC_V3_ADDR_TYPE_DIST;
		break;
	}

	err = ioctl(kvm->vm_fd, KVM_CREATE_DEVICE, &gic_device);
	if (err)
		return err;

	gic_fd = gic_device.fd;

	switch (type) {
	case IRQCHIP_GICV2:
		err = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &cpu_if_attr);
		break;
	case IRQCHIP_GICV3:
		err = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &redist_attr);
		break;
	}
	if (err)
		goto out_err;

	err = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &dist_attr);
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
	int err;

	switch (type) {
	case IRQCHIP_GICV2:
		break;
	case IRQCHIP_GICV3:
		gic_redists_size = kvm->cfg.nrcpus * ARM_GIC_REDIST_SIZE;
		gic_redists_base = ARM_GIC_DIST_BASE - gic_redists_size;
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

	if (!ioctl(gic_fd, KVM_HAS_DEVICE_ATTR, &vgic_init_attr)) {
		ret = ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &vgic_init_attr);
		if (ret)
			return ret;
	}

	return 0;
}
late_init(gic__init_gic)

void gic__generate_fdt_nodes(void *fdt, u32 phandle, enum irqchip_type type)
{
	const char *compatible;
	u64 reg_prop[] = {
		cpu_to_fdt64(ARM_GIC_DIST_BASE), cpu_to_fdt64(ARM_GIC_DIST_SIZE),
		0, 0,				/* to be filled */
	};

	switch (type) {
	case IRQCHIP_GICV2:
		compatible = "arm,cortex-a15-gic";
		reg_prop[2] = cpu_to_fdt64(ARM_GIC_CPUI_BASE);
		reg_prop[3] = cpu_to_fdt64(ARM_GIC_CPUI_SIZE);
		break;
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
	_FDT(fdt_property_cell(fdt, "phandle", phandle));
	_FDT(fdt_end_node(fdt));
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
