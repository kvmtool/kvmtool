#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"

#include "arm-common/gic.h"

#include <linux/byteorder.h>
#include <linux/kvm.h>

static int irq_ids;

int gic__alloc_irqnum(void)
{
	int irq = GIC_SPI_IRQ_BASE + irq_ids++;

	if (irq > GIC_MAX_IRQ)
		die("GIC IRQ limit %d reached!", GIC_MAX_IRQ);

	return irq;
}

int gic__init_irqchip(struct kvm *kvm)
{
	int err;
	struct kvm_device_address gic_addr[] = {
		[0] = {
			.id = (KVM_ARM_DEVICE_VGIC_V2 << KVM_DEVICE_ID_SHIFT) |\
			       KVM_VGIC_V2_ADDR_TYPE_DIST,
			.addr = ARM_GIC_DIST_BASE,
		},
		[1] = {
			.id = (KVM_ARM_DEVICE_VGIC_V2 << KVM_DEVICE_ID_SHIFT) |\
			       KVM_VGIC_V2_ADDR_TYPE_CPU,
			.addr = ARM_GIC_CPUI_BASE,
		}
	};

	if (kvm->nrcpus > GIC_MAX_CPUS) {
		pr_warning("%d CPUS greater than maximum of %d -- truncating\n",
				kvm->nrcpus, GIC_MAX_CPUS);
		kvm->nrcpus = GIC_MAX_CPUS;
	}

	err = ioctl(kvm->vm_fd, KVM_CREATE_IRQCHIP);
	if (err)
		return err;

	err = ioctl(kvm->vm_fd, KVM_SET_DEVICE_ADDRESS, &gic_addr[0]);
	if (err)
		return err;

	err = ioctl(kvm->vm_fd, KVM_SET_DEVICE_ADDRESS, &gic_addr[1]);
	return err;
}

void gic__generate_fdt_nodes(void *fdt, u32 phandle)
{
	u64 reg_prop[] = {
		cpu_to_fdt64(ARM_GIC_DIST_BASE), cpu_to_fdt64(ARM_GIC_DIST_SIZE),
		cpu_to_fdt64(ARM_GIC_CPUI_BASE), cpu_to_fdt64(ARM_GIC_CPUI_SIZE),
	};

	_FDT(fdt_begin_node(fdt, "intc"));
	_FDT(fdt_property_string(fdt, "compatible", "arm,cortex-a15-gic"));
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
