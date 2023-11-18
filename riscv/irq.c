#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/irq.h"
#include "kvm/fdt.h"
#include "kvm/virtio.h"

enum irqchip_type riscv_irqchip = IRQCHIP_UNKNOWN;
bool riscv_irqchip_inkernel;
void (*riscv_irqchip_trigger)(struct kvm *kvm, int irq, int level, bool edge)
				= NULL;
void (*riscv_irqchip_generate_fdt_node)(void *fdt, struct kvm *kvm) = NULL;
u32 riscv_irqchip_phandle = PHANDLE_RESERVED;
u32 riscv_irqchip_msi_phandle = PHANDLE_RESERVED;
bool riscv_irqchip_line_sensing;

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	struct kvm_irq_level irq_level;

	if (riscv_irqchip_inkernel) {
		irq_level.irq = irq;
		irq_level.level = !!level;
		if (ioctl(kvm->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
			pr_warning("%s: Could not KVM_IRQ_LINE for irq %d\n",
				   __func__, irq);
	} else {
		if (riscv_irqchip_trigger)
			riscv_irqchip_trigger(kvm, irq, level, false);
		else
			pr_warning("%s: Can't change level for irq %d\n",
				   __func__, irq);
	}
}

void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	if (riscv_irqchip_inkernel) {
		kvm__irq_line(kvm, irq, VIRTIO_IRQ_HIGH);
		kvm__irq_line(kvm, irq, VIRTIO_IRQ_LOW);
	} else {
		if (riscv_irqchip_trigger)
			riscv_irqchip_trigger(kvm, irq, 1, true);
		else
			pr_warning("%s: Can't trigger irq %d\n",
				   __func__, irq);
	}
}

void riscv__generate_irq_prop(void *fdt, u8 irq, enum irq_type irq_type)
{
	u32 prop[2], size;

	prop[0] = cpu_to_fdt32(irq);
	size = sizeof(u32);
	if (riscv_irqchip_line_sensing) {
		prop[1] = cpu_to_fdt32(irq_type);
		size += sizeof(u32);
	}

	_FDT(fdt_property(fdt, "interrupts", prop, size));
}

void riscv__irqchip_create(struct kvm *kvm)
{
	/* Try PLIC irqchip */
	plic__create(kvm);

	/* Fail if irqchip unknown */
	if (riscv_irqchip == IRQCHIP_UNKNOWN)
		die("No IRQCHIP found\n");
}
