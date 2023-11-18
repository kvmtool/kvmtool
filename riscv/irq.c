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
bool riscv_irqchip_irqfd_ready;

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

struct riscv_irqfd_line {
	unsigned int		gsi;
	int			trigger_fd;
	int			resample_fd;
	struct list_head	list;
};

static LIST_HEAD(irqfd_lines);

int riscv__add_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd,
		     int resample_fd)
{
	struct riscv_irqfd_line *line;

	if (riscv_irqchip_irqfd_ready)
		return irq__common_add_irqfd(kvm, gsi, trigger_fd,
					     resample_fd);

	/* Postpone the routing setup until irqchip is initialized */
	line = malloc(sizeof(*line));
	if (!line)
		return -ENOMEM;

	*line = (struct riscv_irqfd_line) {
		.gsi		= gsi,
		.trigger_fd	= trigger_fd,
		.resample_fd	= resample_fd,
	};
	list_add(&line->list, &irqfd_lines);

	return 0;
}

void riscv__del_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd)
{
	struct riscv_irqfd_line *line;

	if (riscv_irqchip_irqfd_ready) {
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

int riscv__setup_irqfd_lines(struct kvm *kvm)
{
	int ret;
	struct riscv_irqfd_line *line, *tmp;

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

static void (*riscv__irqchip_create_funcs[])(struct kvm *kvm) = {
	aia__create,
	plic__create,
};

void riscv__irqchip_create(struct kvm *kvm)
{
	unsigned int i;

	/* Try irqchip.create function one after another */
	for (i = 0; i < ARRAY_SIZE(riscv__irqchip_create_funcs); i++) {
		riscv__irqchip_create_funcs[i](kvm);
		if (riscv_irqchip != IRQCHIP_UNKNOWN)
			return;
	}

	/* Fail since irqchip is unknown */
	die("No IRQCHIP found\n");
}
